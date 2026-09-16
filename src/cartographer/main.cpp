// Cartographer -- a clean-room re-implementation of the retail TA:Kingdoms map
// editor (see docs/cartographer-port.md). Static-analysis RE of the shipped
// Cartographer.exe drives the behaviour; this shares the engine's rendering
// stack (hpi VFS, tnt loader, terrain compositor, MapView) so terrain looks
// byte-identical to the game.
//
// Working today: mount a retail install, open/create a .tnt map (flat stamp or
// the engine's procedural generator), render the terrain with pan/zoom in the
// editor chrome, paint with the section-prefab stamp brush (sections.h), place
// features (features.h) and units (units.h, via the shared ta::crt), and save.
// triggers.h carries the RE'd condition/action opcode tables, so a rule can be
// read and formatted; authoring them in a dialog, the remaining property
// dialogs, and the HPI/.kmp bundle writer are still to come.
//
// (This said "Phase 0 skeleton ... painting tools, the object model, property
// dialogs and the trigger system land in later phases" long after several of
// those had landed.)

#include <SDL.h>

#include "cartographer/dialog.h"
#include "cartographer/features.h"
#include "cartographer/font5x7.h"
#include "cartographer/newmap.h"
#include "cartographer/sections.h"
#include "cartographer/triggers.h"
#include "cartographer/units.h"
#include "client/mapview.h"
#include "util/appicon.h"
#include "terrain/terrain.h"
#include "util/jpeg.h"
#include "hpi/hpi.h"
#include "tnt/mapgen.h"
#include "tnt/ota.h"
#include "util/png.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace {

constexpr int kMenuH = 22;    // top menu-bar strip
constexpr int kStatusH = 22;  // bottom status strip
// UI magnification. The whole editor is drawn in a fixed logical coordinate
// space and scaled up by this factor via SDL_RenderSetScale, so every panel,
// glyph, and dialog doubles uniformly on hi-DPI displays. Input coordinates are
// divided back down to logical space at the event source.
constexpr int kUIScale = 4;

// A fresh map for the New dialog / --new launch: either a flat ground stamp or
// procedural terrain from the engine's map generator (the "~gen1~" generator the
// lobby uses -- coastlines, relief, trees/rocks/mana, and start positions).
struct FreshMap {
    ta::tnt::Map map;
    std::vector<ta::tnt::StartPos> starts;   // only for random terrain
};
FreshMap buildFreshMap(const ta::hpi::Vfs& vfs, cart::SectionLibrary& sections,
                       ta::terrain::Compositor& comp, const std::string& world,
                       int wUnits, int hUnits, bool random) {
    FreshMap out;
    if (random) {
        ta::mapgen::Params gp;
        static const char* kW[] = {"aramon", "taros", "veruna", "zhon", "creon"};
        gp.mapType = ta::mapgen::Aramon;
        for (uint8_t i = 0; i < 5; ++i) if (world == kW[i]) gp.mapType = i;
        gp.widthCells = uint16_t(wUnits * 32);
        gp.heightCells = uint16_t(hUnits * 32);
        gp.players = 4;
        gp.seed = uint64_t(SDL_GetPerformanceCounter());   // new layout each call
        auto res = ta::mapgen::generate(ta::mapgen::sanitize(gp), vfs);
        out.map = std::move(res.map);
        int n = 1;
        for (auto& [sx, sz] : res.starts) out.starts.push_back({n++, sx, sz});
    } else {
        out.map = cart::newBlankMap(vfs, sections, comp, world, wUnits, hUnits);
    }
    return out;
}

void fillRect(SDL_Renderer* r, int x, int y, int w, int h, Uint8 cr, Uint8 cg, Uint8 cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    SDL_Rect rc{x, y, w, h};
    SDL_RenderFillRect(r, &rc);
}

}  // namespace

int main(int argc, char** argv) {
    std::string dataRoot, mapName, outDir = ".", exportPath, bundlePath, stampName, newWorld = "aramon", shotPath;
    int stampBX = 0, stampBY = 0, newW = 0, newH = 0;
    bool randomTerrain = false;   // --random: generate procedural terrain for --new
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) dataRoot = argv[++i];
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];   // Save destination
        else if (a == "--save" && i + 1 < argc) exportPath = argv[++i];  // headless export+exit
        else if (a == "--bundle" && i + 1 < argc) bundlePath = argv[++i];  // headless: write a .kmp + exit
        else if (a == "--stamp" && i + 3 < argc) {   // headless: stamp <name> <bx> <by>
            stampName = argv[++i]; stampBX = std::atoi(argv[++i]); stampBY = std::atoi(argv[++i]);
        }
        else if (a == "--new" && i + 1 < argc) {   // headless: --new WxH (Units) + --save
            std::sscanf(argv[++i], "%dx%d", &newW, &newH);
        }
        else if (a == "--world" && i + 1 < argc) newWorld = argv[++i];
        else if (a == "--random") randomTerrain = true;   // --new: procedural terrain
        else if (a == "--shot" && i + 1 < argc) shotPath = argv[++i];   // render 1 frame -> PNG
        else if (a[0] != '-') mapName = a;
    }
    // Data root: --data wins; otherwise the local directory if it holds a valid
    // install (drop the editor into a game folder and it just works).
    if (dataRoot.empty()) {
        std::error_code ec;
        std::filesystem::path here = std::filesystem::current_path(ec);
        if (!ec && ta::hpi::validInstall(here, nullptr)) {
            dataRoot = here.string();
            std::fprintf(stderr, "data: using the local directory %s\n", dataRoot.c_str());
        }
    }
    if (dataRoot.empty()) {
        std::fprintf(stderr,
            "Cartographer -- TA:Kingdoms map editor\n"
            "usage: cartographer \"<map name>\" [--data <retail-install-dir>]\n"
            "         (--data is optional when run from inside a game folder)\n"
            "         [--out <dir>]        Ctrl+S / Ctrl+B save destination (default .)\n"
            "         [--new WxH --world <w> [--random]]  start a blank/random map\n"
            "                                 (or press N in-editor)\n"
            "         [--save <file.tnt>]  headless: save loose .tnt/.ota/.crt and exit\n"
            "         [--bundle <file.kmp>] headless: save a packed .kmp map and exit\n"
            "in-editor: Tab tools, 1-5 zoom, G grid, N new, P/R/U/C/T/K scenario menu,\n"
            "           Ctrl+S save loose, Ctrl+B save .kmp\n");
        return 2;
    }

    SDL_SetMainReady();   // we own main() (SDL_MAIN_HANDLED); tell SDL not to hijack it
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow(
        ("Cartographer -- " + mapName).c_str(), SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 1280 * kUIScale, 800 * kUIScale, SDL_WINDOW_RESIZABLE);
    {   // Application icon: the compass-rose badge (src/util/appicon).
        std::vector<uint8_t> ic = ta::appicon::render(ta::appicon::Kind::Cartographer, 64);
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(
                ic.data(), 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32)) {
            SDL_SetWindowIcon(win, s);
            SDL_FreeSurface(s);
        }
    }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    // Draw everything at kUIScale: the logical canvas stays 1280x800-ish while
    // the window is that many times larger, so the UI is magnified uniformly.
    if (ren) SDL_RenderSetScale(ren, float(kUIScale), float(kUIScale));
    if (!win || !ren) {
        std::fprintf(stderr, "window/renderer: %s\n", SDL_GetError());
        return 1;
    }

    // Mount the retail install exactly like the engine (loose + *.hpi, retail
    // precedence). The Vfs must outlive the MapView (it borrows it by ref).
    ta::hpi::Vfs vfs;
    try {
        vfs = ta::hpi::mountRetailRoot(dataRoot, ta::hpi::OverridePolicy::Full);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mount %s: %s\n", dataRoot.c_str(), e.what());
        return 1;
    }
    // --new WxH --save: build a fresh flat map for --world and save it headless.
    // Without --save, --new instead opens the editor on the fresh map (built
    // below), same as pressing N in-editor; --new --bundle exits via that path.
    if (newW > 0 && newH > 0 && !exportPath.empty()) {
        cart::SectionLibrary nsections;
        nsections.scan(vfs, newWorld);
        ta::terrain::Compositor ncomp(vfs);
        ta::tnt::Map nm = cart::newBlankMap(vfs, nsections, ncomp, newWorld, newW, newH);
        if (nm.width == 0) {
            std::fprintf(stderr, "new: no sections for world '%s'\n", newWorld.c_str());
            return 1;
        }
        ta::tnt::Scenario nsc;
        nsc.kingdom = newWorld;
        nsc.missionName = mapName.empty() ? "Untitled" : mapName;
        nsc.sizeW = newW; nsc.sizeH = newH;
        std::string base = exportPath.empty()
            ? (outDir + "/" + (mapName.empty() ? "new" : mapName) + ".tnt") : exportPath;
        std::string otaP = base.substr(0, base.rfind('.')) + ".ota";
        auto put = [](const std::string& p, const void* d, size_t n) {
            std::FILE* f = std::fopen(p.c_str(), "wb");
            if (!f) return false;
            bool ok = std::fwrite(d, 1, n, f) == n; std::fclose(f);
            std::fprintf(stderr, "saved %s (%zu bytes)\n", p.c_str(), n);
            return ok;
        };
        std::vector<uint8_t> tb = nm.save();
        std::string ot = nsc.write();
        bool ok = put(base, tb.data(), tb.size()) & put(otaP, ot.data(), ot.size());
        std::fprintf(stderr, "new: %dx%d Units (%dx%d cells) world=%s\n",
                     newW, newH, nm.width, nm.height, newWorld.c_str());
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return ok ? 0 : 1;
    }

    // Resolve the map name to its .tnt VFS path (Maps/<name>.tnt or a .kmp's
    // kmap/<name>.tnt), same resolution the game uses. An empty name (or --new
    // without --save) starts a fresh blank map instead -- mapPath stays empty so
    // there are no sibling scenario files to load.
    std::string mapPath;
    ta::tnt::Scenario scenario;
    std::unique_ptr<MapView> mapViewPtr;
    if (!mapName.empty()) {
        for (const auto& [name, path] : ta::hpi::listMaps(vfs)) {
            std::string lo = name;
            for (char& c : lo) c = char(std::tolower((unsigned char)c));
            std::string want = mapName;
            for (char& c : want) c = char(std::tolower((unsigned char)c));
            if (lo == want) { mapPath = path; break; }
        }
        if (mapPath.empty()) {
            std::fprintf(stderr, "map '%s' not found in %s\n", mapName.c_str(), dataRoot.c_str());
            return 1;
        }
        mapViewPtr = std::make_unique<MapView>(ren, vfs, mapPath);
        // Companion .ota (metadata + start positions), so a Save round-trips the
        // whole map. Missing/parse-fail = defaults.
        std::string otaPath = mapPath.substr(0, mapPath.rfind('.')) + ".ota";
        try {
            auto b = vfs.read(otaPath);
            scenario = ta::tnt::Scenario::parse(std::string(b.begin(), b.end()));
        } catch (const std::exception&) { /* no .ota: keep defaults */ }
    } else {
        int fw = newW > 0 ? newW : 8, fh = newH > 0 ? newH : 8;
        cart::SectionLibrary ns; ns.scan(vfs, newWorld);
        ta::terrain::Compositor nc(vfs);
        FreshMap fm = buildFreshMap(vfs, ns, nc, newWorld, fw, fh, randomTerrain);
        if (fm.map.width == 0) {
            std::fprintf(stderr, "new: could not build terrain for world '%s'\n", newWorld.c_str());
            return 1;
        }
        mapName = "Untitled";
        scenario.kingdom = newWorld; scenario.sizeW = fw; scenario.sizeH = fh;
        scenario.missionName = mapName;
        scenario.starts = std::move(fm.starts);
        mapViewPtr = std::make_unique<MapView>(ren, vfs, std::move(fm.map));
        SDL_SetWindowTitle(win, "Cartographer -- Untitled");
    }
    MapView& mapView = *mapViewPtr;
    mapView.setBilinear(true);
    std::fprintf(stderr,
                 "cartographer: editing '%s' (%s), %dx%d blocks; scenario '%s' "
                 "kingdom=%s, %zu start positions\n",
                 mapName.c_str(), mapPath.c_str(), mapView.map().blocksX,
                 mapView.map().blocksY, scenario.missionName.c_str(),
                 scenario.kingdom.c_str(), scenario.starts.size());

    auto writeFile = [](const std::string& path, const void* data, size_t n) -> bool {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "save: cannot open %s\n", path.c_str()); return false; }
        size_t w = std::fwrite(data, 1, n, f);
        std::fclose(f);
        std::fprintf(stderr, "saved %s (%zu bytes)\n", path.c_str(), w);
        return w == n;
    };

    // The map's scenario .crt, kept in full so a save preserves the trigger
    // rules, regions, and custom types the unit tool doesn't edit. `units` is
    // the editor's working view (map pixels); `scen` is everything else.
    std::string crtPath = mapPath.substr(0, mapPath.rfind('.')) + ".crt";
    ta::crt::Scenario scen = cart::loadScenario(vfs, crtPath);
    std::vector<cart::PlacedUnit> units = cart::toPlaced(scen);
    bool unitsEdited = false;

    // Use-only restriction: the allowed unit types (UPPERCASE). Loaded from the
    // sibling <map>.tdf when the OTA references one; empty = unrestricted.
    std::set<std::string> useOnly;
    std::string useOnlyTdf = mapPath.substr(0, mapPath.rfind('.')) + ".tdf";
    if (!scenario.useOnlyUnits.empty())
        for (auto& t : cart::loadUseOnly(vfs, useOnlyTdf)) useOnly.insert(t);
    // Set when the terrain is painted, so Save regenerates the minimaps (an
    // unedited save stays byte-identical to the source; an edited one gets a
    // fresh overview reflecting the paint).
    bool edited = false;
    // Unsaved-changes flag for the exit prompt. Set by every edit (terrain,
    // features, units, starts, scenario props, use-only, triggers), cleared on a
    // successful save. Broader than `edited` (which only gates minimap regen).
    bool dirty = false;
    bool wantNew = false;   // deferred "open New Map" after a discard confirmation
    // Save the map as loose <stem>.tnt + <stem>.ota. Loose Maps/<name>.* is
    // read directly by the engine VFS, so a saved map is immediately playable.
    auto saveMap = [&](const std::string& tntPath) -> bool {
        if (edited) {
            std::string wld = scenario.kingdom.empty() ? "aramon" : scenario.kingdom;
            cart::generateMinimaps(mapView.editMap(), mapView.compositor(),
                                   cart::loadWorldPalette(vfs, wld));
        }
        std::vector<uint8_t> tnt = mapView.map().save();
        bool ok = writeFile(tntPath, tnt.data(), tnt.size());
        std::string stem = tntPath.substr(0, tntPath.rfind('.'));
        std::string base = stem.substr(stem.rfind('/') + 1);
        // Use-only restriction: write/clear the sibling .tdf and set the OTA
        // reference BEFORE serializing the OTA (which embeds the filename).
        if (!useOnly.empty()) {
            std::vector<std::string> types(useOnly.begin(), useOnly.end());   // sorted (set)
            std::string tdf = cart::writeUseOnly(types);
            ok &= writeFile(stem + ".tdf", tdf.data(), tdf.size());
            scenario.useOnlyUnits = base + ".tdf";
            // Check-Map warning also fires at save (retail behaviour): placed
            // units whose type is not allowed will not appear in the game.
            int restricted = 0;
            for (const auto& u : units) if (!useOnly.count(u.type)) ++restricted;
            if (restricted)
                std::fprintf(stderr, "check-map: %d placed unit(s) have restricted "
                             "types and will not show up in the game\n", restricted);
        } else {
            scenario.useOnlyUnits.clear();
        }
        std::string otaText = scenario.write();
        ok &= writeFile(stem + ".ota", otaText.data(), otaText.size());
        // The scenario .crt: written whenever the map has (or had) placed units
        // or trigger rules, preserving everything the unit tool doesn't edit.
        if (!units.empty() || !scen.units.empty() || !scen.regions.empty()) {
            std::vector<uint8_t> crt = cart::saveScenario(scen, units);
            ok &= writeFile(stem + ".crt", crt.data(), crt.size());
        }
        return ok;
    };

    // Save the finished map as a single .kmp bundle: an HPI archive holding
    // kmap/<name>/<name>.{tnt,ota,crt,txt} (+ .tdf when restricted), the retail
    // distributable-map format the engine mounts directly.
    auto saveBundle = [&](const std::string& kmpPath) -> bool {
        if (edited) {
            std::string wld = scenario.kingdom.empty() ? "aramon" : scenario.kingdom;
            cart::generateMinimaps(mapView.editMap(), mapView.compositor(),
                                   cart::loadWorldPalette(vfs, wld));
        }
        std::string base = mapName.empty() ? "new" : mapName;
        std::string dir = "kmap/" + base + "/";
        auto bytesOf = [](const std::string& s) {
            return std::vector<uint8_t>(s.begin(), s.end());
        };
        std::vector<ta::hpi::PackFile> pf;
        pf.push_back({dir + base + ".tnt", mapView.map().save()});
        if (!useOnly.empty()) {
            std::vector<std::string> types(useOnly.begin(), useOnly.end());
            pf.push_back({dir + base + ".tdf", bytesOf(cart::writeUseOnly(types))});
            scenario.useOnlyUnits = base + ".tdf";
        } else {
            scenario.useOnlyUnits.clear();
        }
        pf.push_back({dir + base + ".ota", bytesOf(scenario.write())});
        pf.push_back({dir + base + ".crt", cart::saveScenario(scen, units)});
        pf.push_back({dir + base + ".txt", bytesOf(scenario.missionDescription.empty()
                                                       ? scenario.missionName
                                                       : scenario.missionDescription)});
        std::vector<uint8_t> kmp = ta::hpi::pack(pf);
        return writeFile(kmpPath, kmp.data(), kmp.size());
    };

    // Section-prefab palette for this map's world (falls back to aramon).
    cart::SectionLibrary sections;
    std::string world = scenario.kingdom.empty() ? "aramon" : scenario.kingdom;
    sections.scan(vfs, world);
    cart::FeatureLibrary features;
    features.scan(vfs, world);
    std::fprintf(stderr, "cartographer: %zu placeable features for '%s'\n",
                 features.list().size(), world.c_str());

    // Unit-type list for the palette (units + scen loaded above, before saveMap).
    std::vector<std::string> unitTypes = cart::unitTypeNames(vfs);
    std::fprintf(stderr, "cartographer: %zu placed units, %zu unit types\n",
                 units.size(), unitTypes.size());
    std::fprintf(stderr, "cartographer: %zu sections for world '%s'\n",
                 sections.list().size(), world.c_str());

    // Stamp a named section into the map at block (bx,by), returning whether it
    // matched a section. The editor calls this on canvas click; --stamp tests it.
    auto stampByName = [&](const std::string& name, int bx, int by) -> bool {
        for (const auto& s : sections.list())
            if (s.name == name) {
                const ta::tnt::Map* sec = sections.load(vfs, s.path);
                if (sec && cart::stampSection(mapView.editMap(), *sec, bx, by)) {
                    mapView.tilesEdited();
                    edited = true; dirty = true;
                    return true;
                }
            }
        return false;
    };

    // Headless one-shot: --stamp <name> <bx> <by> then save (edit-path test).
    if (!stampName.empty()) {
        bool hit = stampByName(stampName, stampBX, stampBY);
        std::fprintf(stderr, "stamp '%s' at (%d,%d): %s\n", stampName.c_str(),
                     stampBX, stampBY, hit ? "OK" : "no such section");
        bool ok = hit && saveMap(exportPath.empty() ? (outDir + "/" + mapName + "-edit.tnt")
                                                     : exportPath);
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return ok ? 0 : 1;
    }

    // Headless one-shot export: --save <file.tnt> writes and exits (round-trip
    // / convert path, also how the save is regression-tested).
    if (!exportPath.empty()) {
        bool ok = saveMap(exportPath);
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return ok ? 0 : 1;
    }
    // Headless one-shot: --bundle <file.kmp> writes the packed map and exits.
    if (!bundlePath.empty()) {
        bool ok = saveBundle(bundlePath);
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return ok ? 0 : 1;
    }

    // --- Interactive editor state ---------------------------------------------
    constexpr int kPaletteW = 200;   // left section-palette panel
    constexpr int kThumb = 88;       // section thumbnail cell (px)
    const int cols = std::max(1, (kPaletteW - 8) / (kThumb + 4));
    int selected = sections.list().empty() ? -1 : 0;   // section index (TERRAIN)
    int selectedFeat = features.list().empty() ? -1 : 0; // feature index (FEATURES)
    int paletteScroll = 0;
    bool showGrid = false;
    static const float kZoomLevels[5] = {1.0f, 0.75f, 0.5f, 0.25f, 0.125f};

    // Feature-sprite textures (GAF frame 0), cached by feature name, used both in
    // the palette and to draw placed features on the canvas.
    std::map<std::string, SDL_Texture*> featTex;
    auto featTextureFor = [&](const cart::FeatureRef& r) -> SDL_Texture* {
        auto it = featTex.find(r.name);
        if (it != featTex.end()) return it->second;
        SDL_Texture* t = nullptr;
        const cart::FeatSprite* sp = features.sprite(vfs, r);
        if (sp && sp->w > 0) {
            t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                  sp->w, sp->h);
            if (t) {
                SDL_UpdateTexture(t, nullptr, sp->rgba.data(), sp->w * 4);
                SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            }
        }
        featTex[r.name] = t;
        return t;
    };

    // Lazy section thumbnails: render a prefab's terrain (Compositor) once into a
    // small texture, cached by path. Only visible cells ever render.
    std::map<std::string, SDL_Texture*> thumbs;
    auto thumbFor = [&](const std::string& path) -> SDL_Texture* {
        auto it = thumbs.find(path);
        if (it != thumbs.end()) return it->second;
        SDL_Texture* t = nullptr;
        if (const ta::tnt::Map* sec = sections.load(vfs, path)) {
            ta::jpeg::Image img = mapView.compositor().renderMap(*sec);
            if (img.width > 0 && img.height > 0) {
                t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STATIC, img.width, img.height);
                if (t) {
                    SDL_UpdateTexture(t, nullptr, img.rgba.data(), img.width * 4);
                    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
                }
            }
        }
        thumbs[path] = t;   // cache even null (a prefab that won't render)
        return t;
    };

    auto stampAtMouse = [&](int mx, int my, int w, int h) {
        if (selected < 0 || mx < kPaletteW) return;   // palette side, not the canvas
        const auto& s = sections.list()[size_t(selected)];
        const ta::tnt::Map* sec = sections.load(vfs, s.path);
        if (!sec || sec->blocksX <= 0) return;
        // Canvas-local -> world -> block, snapped to the section's own size so
        // sections tile cleanly.
        float lx = float(mx - kPaletteW), ly = float(my - kMenuH);
        int blkX = int((mapView.offX() + lx / mapView.zoom()) / 32.0f);
        int blkY = int((mapView.offY() + ly / mapView.zoom()) / 32.0f);
        int snapX = (blkX / sec->blocksX) * sec->blocksX;
        int snapY = (blkY / sec->blocksY) * sec->blocksY;
        if (cart::stampSection(mapView.editMap(), *sec, snapX, snapY)) {
            mapView.tilesEdited();
            edited = true; dirty = true;
        }
        (void)w; (void)h;
    };

    // --- Object tools (terrain paint, feature placement, start positions) -----
    enum Tool { TERRAIN, FEATURES, UNITS, STARTS };
    int selectedType = unitTypes.empty() ? -1 : 0;   // unit-type index (UNITS palette)
    int currentPlayer = 0;                            // player id new units get
    int draggingUnit = -1;                            // index into units while dragging
    // 8 player marker colours.
    static const Uint8 kPlayerCol[8][3] = {
        {80,120,255},{230,70,60},{80,200,90},{235,210,80},
        {200,90,220},{80,210,220},{240,140,40},{200,200,210}};
    // Placed unit whose marker is near screen (mx,my), or -1.
    auto unitAt = [&](int mx, int my) -> int {
        for (int i = int(units.size()) - 1; i >= 0; --i) {
            float sx = kPaletteW + (units[i].x - mapView.offX()) * mapView.zoom();
            float sy = kMenuH + (units[i].z - mapView.offY()) * mapView.zoom();
            if (std::abs(sx - mx) <= 8 && std::abs(sy - my) <= 8) return i;
        }
        return -1;
    };
    Tool tool = TERRAIN;
    // Place/erase a feature in the .tnt feature plane at the mouse cell.
    auto placeFeature = [&](int mx, int my, bool erase) {
        if (selectedFeat < 0 || mx < kPaletteW) return;
        float wx = mapView.offX() + float(mx - kPaletteW) / mapView.zoom();
        float wz = mapView.offY() + float(my - kMenuH) / mapView.zoom();
        int cx = int(wx / 16.0f), cz = int(wz / 16.0f);
        auto& mp = mapView.editMap();
        if (cx < 0 || cz < 0 || cx >= mp.width || cz >= mp.height) return;
        size_t ci = size_t(cz) * mp.width + cx;
        if (erase) { mp.features[ci] = 0xFFFF; edited = true; dirty = true; return; }
        const std::string& name = features.list()[size_t(selectedFeat)].name;
        uint16_t idx = 0xFFFF;   // intern the feature name into the map's table
        for (size_t i = 0; i < mp.featureNames.size(); ++i)
            if (mp.featureNames[i] == name) { idx = uint16_t(i); break; }
        if (idx == 0xFFFF) { mp.featureNames.push_back(name); idx = uint16_t(mp.featureNames.size() - 1); }
        mp.features[ci] = idx;
        edited = true; dirty = true;
    };
    int draggingStart = -1;           // index into scenario.starts while dragging
    bool clearArm = false, clearDrag = false;   // Clear Area drag-box (screen px)
    int clx0 = 0, cly0 = 0, clx1 = 0, cly1 = 0;
    // Canvas mouse -> map cell (16px). Returns false if off the canvas/map.
    auto mouseCell = [&](int mx, int my, int& cx, int& cz) -> bool {
        if (mx < kPaletteW) return false;
        float wx = mapView.offX() + float(mx - kPaletteW) / mapView.zoom();
        float wz = mapView.offY() + float(my - kMenuH) / mapView.zoom();
        cx = int(wx / 16.0f); cz = int(wz / 16.0f);
        return cx >= 0 && cz >= 0 && cx < mapView.map().width && cz < mapView.map().height;
    };
    // Start position whose marker is near screen (mx,my), or -1.
    auto startAt = [&](int mx, int my) -> int {
        for (int i = 0; i < int(scenario.starts.size()); ++i) {
            float sx = kPaletteW + (scenario.starts[i].xpos * 16.0f - mapView.offX()) * mapView.zoom();
            float sy = kMenuH + (scenario.starts[i].zpos * 16.0f - mapView.offY()) * mapView.zoom();
            if (std::abs(sx - mx) <= 10 && std::abs(sy - my) <= 10) return i;
        }
        return -1;
    };

    // --- Modal dialogs (New, Scenario Properties, Resize, Unit/Rule props, Msg) -
    enum Modal { M_NONE, M_SCENARIO, M_RESIZE, M_UNIT, M_MESSAGE, M_RULE, M_CONFIRM, M_NEW, M_QUITSAVE };
    static constexpr int kMaxFields = 6;
    std::function<void()> confirmAction;   // M_CONFIRM: run on OK
    Modal modal = M_NONE;
    std::string mf[kMaxFields];              // field buffers
    bool mfNumeric[kMaxFields] = {};
    // A field with a non-null choice list is a dropdown (click opens the list),
    // not a typed text box. Used by the New Map dialog for size/world selection.
    const std::vector<std::string>* mfChoices[kMaxFields] = {};
    int mDropOpen = -1;                       // which field's dropdown list is open (-1 none)
    std::vector<SDL_Rect> mDropRects;         // hit rects of the open list's rows
    SDL_Rect mRandom{};                       // New Map: the RANDOM button rect
    // Dropdown option lists (New Map): map sizes in units, and the four worlds.
    const std::vector<std::string> kSizeOpts = {"8", "16", "24", "32", "48", "64"};
    const std::vector<std::string> kWorldOpts = {"aramon", "veruna", "taros", "zhon", "creon"};
    const char* mLabel[kMaxFields] = {};
    std::string mTitle;
    int mN = 0;                              // active field count
    int mfocus = 0;
    int editUnit = -1;                       // UNITS: index being edited (M_UNIT)
    ta::crt::Rule* editRule = nullptr;      // M_RULE: rule whose params are edited
    std::vector<std::string> mMsg;           // M_MESSAGE: wrapped text lines
    SDL_Rect mBox[kMaxFields]{}, mOK{}, mCancel{}, mQuit{};   // render-computed hit rects
    // Pop a message box (word-wrapped to ~46 cols) — used by Check Map.
    auto openMessage = [&](const std::string& title, const std::string& text) {
        mMsg.clear();
        std::string line;
        std::string word;
        auto flush = [&]() { if (!line.empty()) { mMsg.push_back(line); line.clear(); } };
        for (size_t i = 0; i <= text.size(); ++i) {
            char c = i < text.size() ? text[i] : ' ';
            if (c == ' ' || c == '\n') {
                if (line.size() + word.size() + 1 > 46) flush();
                if (!line.empty()) line += ' ';
                line += word; word.clear();
                if (c == '\n') flush();
            } else word += c;
        }
        flush();
        mTitle = title; modal = M_MESSAGE; mN = 0; mfocus = 0;
    };
    // A yes/no confirmation that runs `action` on OK (used by Clear Area).
    auto openConfirm = [&](const std::string& title, const std::string& text,
                           std::function<void()> action) {
        openMessage(title, text);
        modal = M_CONFIRM; confirmAction = std::move(action);
    };
    auto openModal = [&](Modal m, int unitIdx = -1) {
        mfocus = 0; editUnit = unitIdx;
        mDropOpen = -1;
        for (auto& c : mfChoices) c = nullptr;   // default: plain text fields
        if (m == M_SCENARIO) {
            mTitle = "SCENARIO PROPERTIES"; mN = 2;
            mLabel[0] = "SCENARIO NAME"; mf[0] = scenario.missionName;        mfNumeric[0] = false;
            mLabel[1] = "DESCRIPTION";   mf[1] = scenario.missionDescription; mfNumeric[1] = false;
        } else if (m == M_RESIZE) {
            mTitle = "RESIZE MAP"; mN = 2;
            mLabel[0] = "WIDTH (UNITS)";  mf[0] = std::to_string(mapView.map().width / 32);  mfNumeric[0] = true;
            mLabel[1] = "HEIGHT (UNITS)"; mf[1] = std::to_string(mapView.map().height / 32); mfNumeric[1] = true;
        } else if (m == M_NEW) {
            // Name is typed; size + world are dropdowns. RANDOM is a separate
            // button (drawn in the render pass) that generates procedural terrain
            // for the chosen size/world and jumps straight to it.
            mTitle = "NEW MAP"; mN = 4;
            mLabel[0] = "MAP NAME";  mf[0] = "Untitled"; mfNumeric[0] = false;
            mLabel[1] = "WIDTH";     mf[1] = "8";  mfChoices[1] = &kSizeOpts;
            mLabel[2] = "HEIGHT";    mf[2] = "8";  mfChoices[2] = &kSizeOpts;
            std::string w0 = world;
            std::transform(w0.begin(), w0.end(), w0.begin(), ::tolower);
            if (std::find(kWorldOpts.begin(), kWorldOpts.end(), w0) == kWorldOpts.end())
                w0 = "aramon";
            mLabel[3] = "WORLD";     mf[3] = w0;   mfChoices[3] = &kWorldOpts;
        } else if (m == M_UNIT && unitIdx >= 0 && unitIdx < int(units.size())) {
            const auto& u = units[size_t(unitIdx)];
            mTitle = "UNIT PROPERTIES"; mN = 6;
            mLabel[0] = "PLAYER (0-7)";  mf[0] = std::to_string(u.player);      mfNumeric[0] = true;
            mLabel[1] = "HEALTH %";      mf[1] = std::to_string(u.health);      mfNumeric[1] = true;
            mLabel[2] = "ARMOR %";       mf[2] = std::to_string(u.armor);       mfNumeric[2] = true;
            mLabel[3] = "WEAPON %";      mf[3] = std::to_string(u.weapon);      mfNumeric[3] = true;
            mLabel[4] = "VETERAN (0-9)"; mf[4] = std::to_string(u.veteran);     mfNumeric[4] = true;
            mLabel[5] = "ANGLE (DEG)";   mf[5] = std::to_string(int(u.angle));  mfNumeric[5] = true;
        } else {
            return;   // nothing to open
        }
        modal = m;
        SDL_StartTextInput();
    };
    // Build a fresh map from the New Map dialog's fields and switch to it. `random`
    // picks procedural terrain (the RANDOM button) vs a flat stamp (CREATE). On
    // failure it pops a message and leaves the current map untouched; on success it
    // closes the dialog. Shared by the CREATE and RANDOM buttons.
    auto applyNewMap = [&](bool random) {
        std::string nm = mf[0].empty() ? "Untitled" : mf[0];
        int wu = std::clamp(std::atoi(mf[1].c_str()), 1, 64);
        int hu = std::clamp(std::atoi(mf[2].c_str()), 1, 64);
        std::string wld = mf[3];
        std::transform(wld.begin(), wld.end(), wld.begin(), ::tolower);
        if (wld.empty()) wld = "aramon";
        sections.scan(vfs, wld);
        features.scan(vfs, wld);
        FreshMap fm = buildFreshMap(vfs, sections, mapView.compositor(), wld, wu, hu, random);
        if (fm.map.width == 0) {
            SDL_StopTextInput();
            openMessage("NEW MAP", "Could not build a map for world '" + wld +
                        "'. Try aramon, veruna, taros or zhon.");
            return;
        }
        mapView.editMap() = std::move(fm.map);
        mapView.tilesEdited();
        mapView.setOffset(0, 0);
        world = wld;
        scenario = ta::tnt::Scenario{};
        scenario.kingdom = wld; scenario.sizeW = wu; scenario.sizeH = hu;
        scenario.missionName = nm;
        scenario.starts = std::move(fm.starts);
        mapName = nm;
        units.clear(); scen = ta::crt::Scenario{}; useOnly.clear();
        selected = sections.list().empty() ? -1 : 0;
        selectedFeat = features.list().empty() ? -1 : 0;
        paletteScroll = 0;
        edited = true;
        SDL_SetWindowTitle(win, ("Cartographer -- " + mapName).c_str());
        dirty = false;
        modal = M_NONE; SDL_StopTextInput();
    };
    auto applyModal = [&]() {
        if (modal == M_SCENARIO) {
            scenario.missionName = mf[0];
            scenario.missionDescription = mf[1]; dirty = true;
        } else if (modal == M_RESIZE) {
            int wu = std::max(1, std::atoi(mf[0].c_str()));
            int hu = std::max(1, std::atoi(mf[1].c_str()));
            std::string wld = scenario.kingdom.empty() ? "aramon" : scenario.kingdom;
            cart::resizeMap(mapView.editMap(), mapView.compositor(),
                            cart::loadWorldPalette(vfs, wld), wu, hu);
            scenario.sizeW = wu; scenario.sizeH = hu;
            mapView.tilesEdited();
            edited = true; dirty = true;
        } else if (modal == M_NEW) {
            applyNewMap(false);   // CREATE = flat stamp; closes on success
            return;               // (RANDOM is handled at its button click)
        } else if (modal == M_UNIT && editUnit >= 0 && editUnit < int(units.size())) {
            auto& u = units[size_t(editUnit)];
            u.player  = std::clamp(std::atoi(mf[0].c_str()), 0, 7);
            u.health  = std::clamp(std::atoi(mf[1].c_str()), 0, 100);
            u.armor   = std::clamp(std::atoi(mf[2].c_str()), 0, 1000);
            u.weapon  = std::clamp(std::atoi(mf[3].c_str()), 0, 1000);
            u.veteran = std::clamp(std::atoi(mf[4].c_str()), 0, 9);
            int a = std::atoi(mf[5].c_str()) % 360; if (a < 0) a += 360;
            u.angle = float(a);
            unitsEdited = true; dirty = true;
        } else if (modal == M_RULE && editRule) {
            for (int i = 0; i < mN; ++i) editRule->slot[i] = mf[i];
            for (int i = mN; i < 5; ++i) editRule->slot[i].clear();
            editRule = nullptr; dirty = true;
        } else if (modal == M_CONFIRM) {
            if (confirmAction) confirmAction();
            confirmAction = nullptr;
        }
        modal = M_NONE; SDL_StopTextInput();
    };
    // Open the param editor for a condition/action rule (fields = its opcode's
    // parameters, in slot order).
    auto openRuleEditor = [&](ta::crt::Rule* r, bool isAction) {
        const auto& defs = isAction ? cart::actionDefs() : cart::conditionDefs();
        int op = std::clamp(r->opcode, 0, int(defs.size()) - 1);
        const auto& params = defs[size_t(op)].params;
        editRule = r; mfocus = 0; mDropOpen = -1;
        mN = std::min(int(params.size()), kMaxFields);
        mTitle = (isAction ? "ACTION: " : "CONDITION: ") + cart::formatRule(isAction, *r);
        for (int i = 0; i < mN; ++i) {
            mLabel[i] = cart::paramLabel(params[size_t(i)]);
            mf[i] = r->slot[size_t(i)];
            mfNumeric[i] = false;   // slots hold ASCII (numbers, names, flags)
            mfChoices[i] = nullptr; // rule slots are typed, not dropdowns
        }
        modal = M_RULE;
        if (mN > 0) SDL_StartTextInput();
    };

    // --- Use Only restriction list + Check Map (phase 4) ----------------------
    bool useOnlyOpen = false;
    int useOnlyScroll = 0;
    SDL_Rect uoList{}, uoDone{}, uoClear{};   // render-computed hit rects
    // Check Map: retail warns only about placed units whose type is restricted.
    auto checkMap = [&]() {
        if (useOnly.empty()) {
            openMessage("CHECK MAP", "No unit-type restriction is set (Use Only is "
                        "empty), so every placed unit will appear in the game.");
            return;
        }
        std::vector<std::string> bad;
        std::set<std::string> seen;
        for (const auto& u : units)
            if (!useOnly.count(u.type) && seen.insert(u.type).second) bad.push_back(u.type);
        if (bad.empty()) {
            openMessage("CHECK MAP", "Map OK: every placed unit's type is in the "
                        "Use Only list.");
            return;
        }
        std::string list;
        for (size_t i = 0; i < bad.size(); ++i) { if (i) list += ", "; list += bad[i]; }
        openMessage("CHECK MAP", std::to_string(bad.size()) + " placed unit type(s) "
                    "have been restricted and will not show up in the game: " + list);
    };

    // --- Scenario Scripting (per-player trigger rules) overlay (phase 5) ------
    bool scriptOpen = false;
    int scrPlayer = 0;                     // 0..8
    int scrGroup = -1;                     // selected rule-group in this player
    int scrCondSel = -1, scrActSel = -1;   // selected condition / action row
    int scrRuleScroll = 0, scrCondScroll = 0, scrActScroll = 0;
    bool pickOpen = false, pickAction = false;   // opcode picker (add cond/act)
    int pickScroll = 0;
    SDL_Rect rRuleList{}, rCondList{}, rActList{}, rPickList{};   // render-computed
    SDL_Rect rPrevP{}, rNextP{}, rAddRule{}, rDelRule{}, rAddCond{}, rDelCond{},
             rAddAct{}, rDelAct{}, rScrDone{}, rPickCancel{};
    auto scrGroups = [&]() -> std::vector<ta::crt::RuleGroup>& {
        return scen.players[size_t(scrPlayer)];
    };
    auto curGroup = [&]() -> ta::crt::RuleGroup* {
        auto& gs = scrGroups();
        return (scrGroup >= 0 && scrGroup < int(gs.size())) ? &gs[size_t(scrGroup)] : nullptr;
    };
    auto openScripting = [&]() {
        if (int(scen.players.size()) < 9) scen.players.resize(9);   // retail writes 9
        scriptOpen = true; scrPlayer = 0;
        scrGroup = scen.players[0].empty() ? -1 : 0;
        scrCondSel = scrActSel = -1;
        scrRuleScroll = scrCondScroll = scrActScroll = 0;
    };

    if (!shotPath.empty()) {
        mapView.setZoom(0.3f);          // fit-ish view for the shot
        mapView.finishChunks();         // wait for the terrain to decode+upload
    }
    // A bare launch (no map named, no --new size) opens on the blank map with
    // the New Map dialog already up, so the first thing is "what shall we make?".
    if (mapPath.empty() && newW == 0 && shotPath.empty()) openModal(M_NEW);
    bool running = true;
    while (running) {
        int w, h;
        SDL_GetRendererOutputSize(ren, &w, &h);
        w /= kUIScale; h /= kUIScale;   // physical -> logical (SDL_RenderSetScale)
        int canvasH = h - kMenuH - kStatusH;
        int canvasW = w - kPaletteW;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            // Map input from physical window pixels into the logical draw space.
            if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                e.button.x /= kUIScale; e.button.y /= kUIScale;
            } else if (e.type == SDL_MOUSEMOTION) {
                e.motion.x /= kUIScale; e.motion.y /= kUIScale;
                e.motion.xrel /= kUIScale; e.motion.yrel /= kUIScale;
            }
            if (e.type == SDL_QUIT) {
                if (dirty) {
                    // Close any transient overlay so the quit prompt gets input.
                    useOnlyOpen = false; scriptOpen = false; pickOpen = false;
                    modal = M_QUITSAVE; SDL_StopTextInput();
                } else running = false;
                continue;
            }
            // The Use Only checklist overlay swallows input while up.
            if (useOnlyOpen) {
                if (e.type == SDL_MOUSEWHEEL) {
                    int maxS = std::max(0, int(unitTypes.size()) * 14 - uoList.h);
                    useOnlyScroll = std::clamp(useOnlyScroll - e.wheel.y * 40, 0, maxS);
                } else if (e.type == SDL_KEYDOWN &&
                           (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_RETURN)) {
                    useOnlyOpen = false;
                } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    int mx = e.button.x, my = e.button.y;
                    if (cart::pointIn(mx, my, uoDone)) useOnlyOpen = false;
                    else if (cart::pointIn(mx, my, uoClear)) { useOnly.clear(); dirty = true; }
                    else if (cart::pointIn(mx, my, uoList)) {
                        int row = (my - uoList.y + useOnlyScroll) / 14;
                        if (row >= 0 && row < int(unitTypes.size())) {
                            const std::string& t = unitTypes[size_t(row)];
                            if (useOnly.count(t)) useOnly.erase(t); else useOnly.insert(t); dirty = true;
                        }
                    }
                }
                continue;
            }
            // A modal dialog swallows all input while up.
            if (modal != M_NONE) {
                // Save-before-exit prompt: SAVE / DON'T SAVE / CANCEL.
                if (modal == M_QUITSAVE) {
                    auto saveThenQuit = [&]() {
                        // Only exit if the save actually succeeded -- otherwise
                        // keep the editor alive and say so, so a failed write
                        // (read-only dir, disk full) can't silently lose work.
                        if (saveMap(outDir + "/" + mapName + ".tnt")) {
                            dirty = false; running = false; modal = M_NONE;
                        } else {
                            openMessage("SAVE FAILED",
                                        "Could not write the map; your changes were NOT saved.");
                        }
                    };
                    if (e.type == SDL_KEYDOWN) {
                        SDL_Keycode k = e.key.keysym.sym;
                        if (k == SDLK_ESCAPE) modal = M_NONE;                    // cancel
                        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) saveThenQuit();
                    } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                        int mx = e.button.x, my = e.button.y;
                        if (cart::pointIn(mx, my, mOK)) saveThenQuit();
                        else if (cart::pointIn(mx, my, mQuit)) { running = false; modal = M_NONE; }
                        else if (cart::pointIn(mx, my, mCancel)) modal = M_NONE;
                    }
                    continue;
                }
                if (e.type == SDL_TEXTINPUT && mN > 0 && !mfChoices[mfocus]) {
                    for (const char* c = e.text.text; *c; ++c)
                        if (!mfNumeric[mfocus] || (*c >= '0' && *c <= '9')) mf[mfocus] += *c;
                } else if (e.type == SDL_KEYDOWN) {
                    SDL_Keycode k = e.key.keysym.sym;
                    if (k == SDLK_BACKSPACE && mN > 0 && !mfChoices[mfocus] && !mf[mfocus].empty())
                        mf[mfocus].pop_back();
                    else if (k == SDLK_TAB) { mfocus = (mfocus + 1) % std::max(1, mN); mDropOpen = -1; }
                    else if ((k == SDLK_LEFT || k == SDLK_RIGHT) && mN > 0 && mfChoices[mfocus]) {
                        // Cycle a focused dropdown field with the arrow keys.
                        const auto& opts = *mfChoices[mfocus];
                        int cur = 0;
                        for (int j = 0; j < int(opts.size()); ++j)
                            if (opts[size_t(j)] == mf[mfocus]) { cur = j; break; }
                        cur = (cur + (k == SDLK_RIGHT ? 1 : int(opts.size()) - 1)) % int(opts.size());
                        mf[mfocus] = opts[size_t(cur)];
                    }
                    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) applyModal();
                    else if (k == SDLK_ESCAPE) {
                        if (mDropOpen >= 0) mDropOpen = -1;   // first Esc closes an open list
                        else { modal = M_NONE; SDL_StopTextInput(); }
                    }
                } else if (e.type == SDL_MOUSEBUTTONDOWN &&
                           e.button.button == SDL_BUTTON_LEFT) {
                    int mx = e.button.x, my = e.button.y;
                    // An open dropdown list eats the next click: a row selects, any
                    // click closes it (so a stray click just dismisses the list).
                    if (mDropOpen >= 0) {
                        for (size_t i = 0; i < mDropRects.size(); ++i)
                            if (cart::pointIn(mx, my, mDropRects[i])) {
                                mf[mDropOpen] = (*mfChoices[mDropOpen])[i]; break;
                            }
                        mDropOpen = -1;
                        continue;
                    }
                    if (cart::pointIn(mx, my, mOK)) { applyModal(); continue; }
                    if (modal == M_NEW && cart::pointIn(mx, my, mRandom)) { applyNewMap(true); continue; }
                    if (cart::pointIn(mx, my, mCancel)) { modal = M_NONE; SDL_StopTextInput(); continue; }
                    // A choice field opens its dropdown; a text field takes focus.
                    for (int i = 0; i < mN; ++i)
                        if (cart::pointIn(mx, my, mBox[i])) {
                            if (mfChoices[i]) { mDropOpen = i; mfocus = i; }
                            else mfocus = i;
                            break;
                        }
                }
                continue;
            }
            // The Scripting (trigger) overlay swallows input while up.
            if (scriptOpen) {
                constexpr int kRow = 12;
                // Nested opcode picker (choose a condition/action type to add).
                if (pickOpen) {
                    const auto& defs = pickAction ? cart::actionDefs() : cart::conditionDefs();
                    if (e.type == SDL_MOUSEWHEEL) {
                        int maxS = std::max(0, int(defs.size()) * kRow - rPickList.h);
                        pickScroll = std::clamp(pickScroll - e.wheel.y * 36, 0, maxS);
                    } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                        pickOpen = false;
                    } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                        int mx = e.button.x, my = e.button.y;
                        if (cart::pointIn(mx, my, rPickCancel)) pickOpen = false;
                        else if (cart::pointIn(mx, my, rPickList)) {
                            int row = (my - rPickList.y + pickScroll) / kRow;
                            ta::crt::RuleGroup* g = curGroup();
                            if (g && row >= 0 && row < int(defs.size())) {
                                ta::crt::Rule r; r.opcode = row;
                                for (size_t i = 0; i < defs[size_t(row)].params.size() && i < 5; ++i)
                                    r.slot[i] = cart::defaultParam(defs[size_t(row)].params[i]);
                                if (pickAction) { g->actions.push_back(r); scrActSel = int(g->actions.size()) - 1; dirty = true; }
                                else { g->conditions.push_back(r); scrCondSel = int(g->conditions.size()) - 1; dirty = true; }
                                pickOpen = false;
                            }
                        }
                    }
                    continue;
                }
                auto& gs = scrGroups();
                ta::crt::RuleGroup* g = curGroup();
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                    scriptOpen = false;
                } else if (e.type == SDL_MOUSEWHEEL) {
                    int mx, my; SDL_GetMouseState(&mx, &my); mx /= kUIScale; my /= kUIScale;
                    if (cart::pointIn(mx, my, rRuleList)) {
                        int maxS = std::max(0, int(gs.size()) * kRow - rRuleList.h);
                        scrRuleScroll = std::clamp(scrRuleScroll - e.wheel.y * 36, 0, maxS);
                    } else if (g && cart::pointIn(mx, my, rCondList)) {
                        int maxS = std::max(0, int(g->conditions.size()) * kRow - rCondList.h);
                        scrCondScroll = std::clamp(scrCondScroll - e.wheel.y * 36, 0, maxS);
                    } else if (g && cart::pointIn(mx, my, rActList)) {
                        int maxS = std::max(0, int(g->actions.size()) * kRow - rActList.h);
                        scrActScroll = std::clamp(scrActScroll - e.wheel.y * 36, 0, maxS);
                    }
                } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    int mx = e.button.x, my = e.button.y;
                    bool dbl = e.button.clicks >= 2;
                    if (cart::pointIn(mx, my, rScrDone)) scriptOpen = false;
                    else if (cart::pointIn(mx, my, rPrevP)) {
                        scrPlayer = (scrPlayer + 8) % 9; scrGroup = scen.players[size_t(scrPlayer)].empty() ? -1 : 0;
                        scrCondSel = scrActSel = -1; scrRuleScroll = scrCondScroll = scrActScroll = 0;
                    } else if (cart::pointIn(mx, my, rNextP)) {
                        scrPlayer = (scrPlayer + 1) % 9; scrGroup = scen.players[size_t(scrPlayer)].empty() ? -1 : 0;
                        scrCondSel = scrActSel = -1; scrRuleScroll = scrCondScroll = scrActScroll = 0;
                    } else if (cart::pointIn(mx, my, rAddRule)) {
                        gs.push_back({}); scrGroup = int(gs.size()) - 1; scrCondSel = scrActSel = -1; dirty = true;
                    } else if (cart::pointIn(mx, my, rDelRule) && g) {
                        gs.erase(gs.begin() + scrGroup); dirty = true;
                        scrGroup = gs.empty() ? -1 : std::min(scrGroup, int(gs.size()) - 1);
                        scrCondSel = scrActSel = -1;
                    } else if (cart::pointIn(mx, my, rAddCond)) {
                        if (!g) { gs.push_back({}); scrGroup = int(gs.size()) - 1; dirty = true; }
                        pickOpen = true; pickAction = false; pickScroll = 0;
                    } else if (cart::pointIn(mx, my, rAddAct)) {
                        if (!g) { gs.push_back({}); scrGroup = int(gs.size()) - 1; dirty = true; }
                        pickOpen = true; pickAction = true; pickScroll = 0;
                    } else if (cart::pointIn(mx, my, rDelCond) && g && scrCondSel >= 0 &&
                               scrCondSel < int(g->conditions.size())) {
                        g->conditions.erase(g->conditions.begin() + scrCondSel); scrCondSel = -1; dirty = true;
                    } else if (cart::pointIn(mx, my, rDelAct) && g && scrActSel >= 0 &&
                               scrActSel < int(g->actions.size())) {
                        g->actions.erase(g->actions.begin() + scrActSel); scrActSel = -1; dirty = true;
                    } else if (cart::pointIn(mx, my, rRuleList)) {
                        int row = (my - rRuleList.y + scrRuleScroll) / kRow;
                        if (row >= 0 && row < int(gs.size())) {
                            scrGroup = row; scrCondSel = scrActSel = -1;
                            scrCondScroll = scrActScroll = 0;
                        }
                    } else if (g && cart::pointIn(mx, my, rCondList)) {
                        int row = (my - rCondList.y + scrCondScroll) / kRow;
                        if (row >= 0 && row < int(g->conditions.size())) {
                            scrCondSel = row;
                            if (dbl) openRuleEditor(&g->conditions[size_t(row)], false);
                        }
                    } else if (g && cart::pointIn(mx, my, rActList)) {
                        int row = (my - rActList.y + scrActScroll) / kRow;
                        if (row >= 0 && row < int(g->actions.size())) {
                            scrActSel = row;
                            if (dbl) openRuleEditor(&g->actions[size_t(row)], true);
                        }
                    }
                }
                continue;
            }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                if (dirty) {
                    // Close any transient overlay so the quit prompt gets input.
                    useOnlyOpen = false; scriptOpen = false; pickOpen = false;
                    modal = M_QUITSAVE; SDL_StopTextInput();
                } else running = false;
            } else if (e.type == SDL_KEYDOWN && (e.key.keysym.mod & KMOD_CTRL) &&
                     e.key.keysym.sym == SDLK_s) {
                bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
                // Shift+S writes a "-edit" side copy; only a save of the canonical
                // map file clears the unsaved-changes flag.
                if (saveMap(outDir + "/" + mapName + (shift ? "-edit" : "") + ".tnt") && !shift)
                    dirty = false;
            } else if (e.type == SDL_KEYDOWN && (e.key.keysym.mod & KMOD_CTRL) &&
                       e.key.keysym.sym == SDLK_b) {
                // Ctrl+B: save the finished map as a single .kmp bundle.
                bool ok = saveBundle(outDir + "/" + mapName + ".kmp");
                if (ok) dirty = false;
                openMessage("SAVE BUNDLE", ok ? ("Saved " + mapName + ".kmp")
                                              : "Could not write the .kmp bundle.");
            } else if (e.type == SDL_KEYDOWN && (e.key.keysym.mod & KMOD_CTRL) &&
                       e.key.keysym.sym == SDLK_l) {
                // Land Lasso: toggle between land (terrain-stamp) and object mode.
                tool = tool == TERRAIN ? FEATURES : TERRAIN;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_k) {
                clearArm = !clearArm;    // Edit -> Clear Area (drag a box)
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_n) {
                // File -> New Map. Guard unsaved edits (New replaces the whole map).
                if (dirty) openConfirm("NEW MAP", "Discard unsaved changes and start "
                                       "a new map?", [&]() { wantNew = true; });
                else openModal(M_NEW);
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_p) {
                openModal(M_SCENARIO);   // Scenario -> Properties
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_r) {
                openModal(M_RESIZE);     // Scenario -> Resize
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_u) {
                useOnlyOpen = true;      // Scenario -> Use Only (unit restriction)
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_c) {
                checkMap();              // Scenario -> Check Map
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_t) {
                openScripting();         // Scenario -> Scripting (triggers)
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_LEFTBRACKET) {
                currentPlayer = (currentPlayer + 7) % 8;   // UNITS: pick placement player
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RIGHTBRACKET) {
                currentPlayer = (currentPlayer + 1) % 8;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_g) {
                showGrid = !showGrid;   // View -> Grid
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_TAB) {
                tool = Tool((int(tool) + 1) % 4);   // cycle TERRAIN->FEATURES->UNITS->STARTS
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym >= SDLK_1 &&
                       e.key.keysym.sym <= SDLK_5) {
                // Zoom levels 1..5 = 100/75/50/25/12.5% (retail's five steps).
                mapView.setZoom(kZoomLevels[e.key.keysym.sym - SDLK_1]);
            } else if (e.type == SDL_MOUSEBUTTONDOWN &&
                       e.button.button == SDL_BUTTON_LEFT && e.button.y < kMenuH) {
                // Toolbar buttons: TERRAIN | FEATURES | STARTS (each 72px).
                int bi = (e.button.x - 96) / 72;
                if (bi >= 0 && bi < 4) tool = Tool(bi);
            } else if (e.type == SDL_MOUSEBUTTONDOWN &&
                       e.button.button == SDL_BUTTON_LEFT && e.button.x < kPaletteW &&
                       e.button.y >= kMenuH && e.button.y < h - kStatusH) {
                // Palette click -> select a section/feature (thumbnails) or a unit
                // type (UNITS = a text list, 14px rows).
                if (tool == UNITS) {
                    int row = (e.button.y - kMenuH + paletteScroll) / 14;
                    if (row >= 0 && row < int(unitTypes.size())) selectedType = row;
                } else {
                    int px = e.button.x - 4;
                    int py = e.button.y - kMenuH + paletteScroll;
                    int col = px / (kThumb + 4), row = py / (kThumb + 4);
                    if (col >= 0 && col < cols) {
                        int idx = row * cols + col;
                        if (tool == FEATURES) {
                            if (idx >= 0 && idx < int(features.list().size())) selectedFeat = idx;
                        } else if (idx >= 0 && idx < int(sections.list().size())) selected = idx;
                    }
                }
            } else if (e.type == SDL_MOUSEWHEEL) {
                int mxp, myp; SDL_GetMouseState(&mxp, &myp); mxp /= kUIScale; myp /= kUIScale;
                if (mxp < kPaletteW) {   // scroll the palette (tool-dependent count)
                    if (tool == UNITS) {
                        int maxS = std::max(0, int(unitTypes.size()) * 14 - canvasH);
                        paletteScroll = std::clamp(paletteScroll - e.wheel.y * 40, 0, maxS);
                        continue;
                    }
                    int n = tool == FEATURES ? int(features.list().size())
                                             : int(sections.list().size());
                    int rows = (n + cols - 1) / cols;
                    int maxScroll = std::max(0, rows * (kThumb + 4) - canvasH);
                    paletteScroll = std::clamp(paletteScroll - e.wheel.y * 40, 0, maxScroll);
                } else {
                    mapView.input(e);   // zoom the canvas
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN &&
                       e.button.button == SDL_BUTTON_LEFT && e.button.x >= kPaletteW) {
                if (clearArm) {   // Clear Area: begin the drag box
                    clearDrag = true;
                    clx0 = clx1 = e.button.x; cly0 = cly1 = e.button.y;
                } else if (tool == TERRAIN) {
                    stampAtMouse(e.button.x, e.button.y, canvasW, canvasH);
                } else if (tool == FEATURES) {
                    placeFeature(e.button.x, e.button.y, false);
                } else if (tool == UNITS) {   // grab an existing unit, else place one
                    int hit = unitAt(e.button.x, e.button.y);
                    int cx, cz;
                    if (hit >= 0 && e.button.clicks >= 2) {
                        openModal(M_UNIT, hit);   // double-click: edit properties
                    } else if (hit >= 0) {
                        draggingUnit = hit;
                    } else if (selectedType >= 0 && mouseCell(e.button.x, e.button.y, cx, cz)) {
                        cart::PlacedUnit u;
                        u.type = unitTypes[size_t(selectedType)];
                        u.player = currentPlayer;
                        u.x = cx * 16.0f + 8; u.z = cz * 16.0f + 8;
                        units.push_back(u);
                        draggingUnit = int(units.size()) - 1;
                        unitsEdited = true; dirty = true;
                    }
                } else {   // STARTS: grab an existing marker, else place a new one
                    int hit = startAt(e.button.x, e.button.y);
                    int cx, cz;
                    if (hit >= 0) draggingStart = hit;
                    else if (mouseCell(e.button.x, e.button.y, cx, cz)) {
                        int num = 1;   // next free StartPosN
                        for (bool used = true; used; ++num) {
                            used = false;
                            for (auto& s : scenario.starts) if (s.number == num) used = true;
                        }
                        scenario.starts.push_back({num - 1, cx, cz}); dirty = true;
                        draggingStart = int(scenario.starts.size()) - 1;
                    }
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN &&
                       e.button.button == SDL_BUTTON_RIGHT && tool == STARTS) {
                int hit = startAt(e.button.x, e.button.y);   // right-click deletes a start
                if (hit >= 0) { scenario.starts.erase(scenario.starts.begin() + hit); dirty = true; }
            } else if (e.type == SDL_MOUSEBUTTONDOWN &&
                       e.button.button == SDL_BUTTON_RIGHT && tool == FEATURES &&
                       e.button.x >= kPaletteW) {
                placeFeature(e.button.x, e.button.y, true);   // right-click erases
            } else if (e.type == SDL_MOUSEBUTTONDOWN &&
                       e.button.button == SDL_BUTTON_RIGHT && tool == UNITS) {
                int hit = unitAt(e.button.x, e.button.y);   // right-click deletes a unit
                if (hit >= 0) { units.erase(units.begin() + hit); unitsEdited = true; dirty = true; }
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                draggingStart = -1; draggingUnit = -1;
                if (clearDrag) {   // Clear Area: confirm, then remove units + features
                    clearDrag = false;
                    int a0, b0, a1, b1;
                    if (mouseCell(std::min(clx0, clx1), std::min(cly0, cly1), a0, b0) &&
                        mouseCell(std::max(clx0, clx1), std::max(cly0, cly1), a1, b1)) {
                        int lox = std::min(a0, a1), hix = std::max(a0, a1);
                        int loz = std::min(b0, b1), hiz = std::max(b0, b1);
                        int nUnits = 0;
                        for (const auto& u : units) {
                            int ux = int(u.x / 16.0f), uz = int(u.z / 16.0f);
                            if (ux >= lox && ux <= hix && uz >= loz && uz <= hiz) ++nUnits;
                        }
                        int cells = (hix - lox + 1) * (hiz - loz + 1);
                        openConfirm("CLEAR AREA",
                                    "Remove all units and features in this area?  (" +
                                        std::to_string(nUnits) + " unit(s), " +
                                        std::to_string(cells) + " cells)",
                                    [&units, &mapView, &edited, &unitsEdited, &dirty,
                                     lox, hix, loz, hiz]() {
                            auto& m = mapView.editMap();
                            for (int z = loz; z <= hiz; ++z)
                                for (int x = lox; x <= hix; ++x)
                                    m.features[size_t(z) * m.width + x] = 0xFFFF;
                            units.erase(std::remove_if(units.begin(), units.end(),
                                [&](const cart::PlacedUnit& u) {
                                    int ux = int(u.x / 16.0f), uz = int(u.z / 16.0f);
                                    return ux >= lox && ux <= hix && uz >= loz && uz <= hiz;
                                }), units.end());
                            mapView.tilesEdited(); edited = true; unitsEdited = true; dirty = true;
                        });
                    }
                    clearArm = false;
                }
            } else if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK) &&
                       e.motion.x >= kPaletteW) {
                if (clearDrag) {   // Clear Area: grow the box
                    clx1 = e.motion.x; cly1 = e.motion.y;
                } else if (tool == TERRAIN) {
                    stampAtMouse(e.motion.x, e.motion.y, canvasW, canvasH);   // drag-paint
                } else if (tool == FEATURES) {
                    placeFeature(e.motion.x, e.motion.y, false);   // drag-place features
                } else if (tool == UNITS && draggingUnit >= 0) {
                    int cx, cz;   // drag a unit to a new cell centre
                    if (mouseCell(e.motion.x, e.motion.y, cx, cz)) {
                        units[size_t(draggingUnit)].x = cx * 16.0f + 8;
                        units[size_t(draggingUnit)].z = cz * 16.0f + 8;
                        unitsEdited = true; dirty = true;
                    }
                } else if (draggingStart >= 0) {
                    int cx, cz;   // drag a start marker to a new cell
                    if (mouseCell(e.motion.x, e.motion.y, cx, cz)) {
                        scenario.starts[size_t(draggingStart)].xpos = cx;
                        scenario.starts[size_t(draggingStart)].zpos = cz; dirty = true;
                    }
                }
            } else if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_RMASK)) {
                if (tool == FEATURES && e.motion.x >= kPaletteW) {
                    placeFeature(e.motion.x, e.motion.y, true);   // right-drag erases
                } else {
                    // Right-drag pans; feed MapView a synthetic left-drag motion.
                    SDL_Event pan = e;
                    pan.motion.state = SDL_BUTTON_LMASK;
                    mapView.input(pan);
                }
            } else if (e.type != SDL_MOUSEBUTTONDOWN && e.type != SDL_MOUSEMOTION) {
                mapView.input(e);
            }
        }

        // A confirmed New (discarding unsaved edits) opens the dialog now, after
        // the confirm's applyModal has closed itself.
        if (wantNew) { wantNew = false; openModal(M_NEW); }

        mapView.ensureChunks(canvasW, canvasH);

        SDL_SetRenderDrawColor(ren, 24, 26, 32, 255);
        SDL_RenderClear(ren);

        // Map canvas (right of the palette, between menu and status).
        SDL_Rect canvas{kPaletteW, kMenuH, canvasW, canvasH};
        SDL_RenderSetViewport(ren, &canvas);
        mapView.draw(canvasW, canvasH);
        // Grid overlay: section (512px) lines bright, block (32px) lines faint.
        if (showGrid) {
            float zm = mapView.zoom();
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            int firstBlkX = int(mapView.offX()) / 32, firstBlkY = int(mapView.offY()) / 32;
            for (int bxg = firstBlkX; ; ++bxg) {
                float sx = (bxg * 32 - mapView.offX()) * zm;
                if (sx > canvasW) break;
                bool section = (bxg % 16) == 0;
                SDL_SetRenderDrawColor(ren, 255, 255, 255, section ? 90 : 30);
                SDL_RenderDrawLine(ren, int(sx), 0, int(sx), canvasH);
            }
            for (int byg = firstBlkY; ; ++byg) {
                float sy = (byg * 32 - mapView.offY()) * zm;
                if (sy > canvasH) break;
                bool section = (byg % 16) == 0;
                SDL_SetRenderDrawColor(ren, 255, 255, 255, section ? 90 : 30);
                SDL_RenderDrawLine(ren, 0, int(sy), canvasW, int(sy));
            }
        }
        // Placed features: draw each non-empty feature-plane cell's sprite at its
        // cell, anchored like the game. Culled to the visible canvas.
        {
            const auto& mp = mapView.map();
            float zm = mapView.zoom();
            for (int cz = 0; cz < mp.height; ++cz)
                for (int cx = 0; cx < mp.width; ++cx) {
                    uint16_t v = mp.features[size_t(cz) * mp.width + cx];
                    if (v >= 0xFFFA || v >= mp.featureNames.size()) continue;
                    float bx = (cx * 16.0f - mapView.offX()) * zm;
                    float by = (cz * 16.0f - mapView.offY()) * zm;
                    if (bx < -64 || by < -64 || bx > canvasW + 64 || by > canvasH + 64) continue;
                    const cart::FeatureRef* r = features.byName(mp.featureNames[v]);
                    if (!r) continue;
                    SDL_Texture* t = featTextureFor(*r);
                    const cart::FeatSprite* sp = features.sprite(vfs, *r);
                    if (!t || !sp || sp->w == 0) {   // no art: a small marker
                        SDL_SetRenderDrawColor(ren, 90, 200, 90, 220);
                        SDL_Rect dot{int(bx) - 2, int(by) - 2, 4, 4};
                        SDL_RenderFillRect(ren, &dot);
                        continue;
                    }
                    SDL_FRect dst{bx - sp->xoff * zm, by - sp->yoff * zm,
                                  sp->w * zm, sp->h * zm};
                    SDL_RenderCopyF(ren, t, nullptr, &dst);
                }
        }
        // Placed units: a player-coloured square with the type name above it.
        for (int i = 0; i < int(units.size()); ++i) {
            float sx = (units[i].x - mapView.offX()) * mapView.zoom();
            float sy = (units[i].z - mapView.offY()) * mapView.zoom();
            if (sx < -20 || sy < -20 || sx > canvasW + 20 || sy > canvasH + 20) continue;
            const Uint8* pc = kPlayerCol[units[i].player & 7];
            SDL_SetRenderDrawColor(ren, pc[0], pc[1], pc[2], 255);
            SDL_Rect r{int(sx) - 5, int(sy) - 5, 10, 10};
            SDL_RenderFillRect(ren, &r);
            SDL_SetRenderDrawColor(ren, 12, 12, 16, 255);
            SDL_RenderDrawRect(ren, &r);
            if (mapView.zoom() > 0.28f)   // label only when there's room
                cart::drawText(ren, units[i].type,
                               int(sx) - cart::textWidth(units[i].type, 1) / 2,
                               int(sy) - 15, 1, 235, 235, 245);
        }
        // Start-position markers (drawn in canvas-local coords: gold diamonds
        // with the StartPos number). Off-map ones simply fall outside.
        for (int i = 0; i < int(scenario.starts.size()); ++i) {
            float sx = (scenario.starts[i].xpos * 16.0f - mapView.offX()) * mapView.zoom();
            float sy = (scenario.starts[i].zpos * 16.0f - mapView.offY()) * mapView.zoom();
            if (sx < -12 || sy < -12 || sx > canvasW + 12 || sy > canvasH + 12) continue;
            SDL_Vertex d[4] = {
                {{sx, sy - 9}, {255, 205, 70, 255}, {0, 0}},
                {{sx + 9, sy}, {255, 205, 70, 255}, {0, 0}},
                {{sx, sy + 9}, {255, 205, 70, 255}, {0, 0}},
                {{sx - 9, sy}, {255, 205, 70, 255}, {0, 0}},
            };
            int di[6] = {0, 1, 2, 0, 2, 3};
            SDL_RenderGeometry(ren, nullptr, d, 4, di, 6);
            SDL_SetRenderDrawColor(ren, 30, 20, 0, 255);
            SDL_Rect out{int(sx) - 9, int(sy) - 9, 18, 18};
            SDL_RenderDrawRect(ren, &out);
            cart::drawText(ren, std::to_string(scenario.starts[i].number),
                           int(sx) - 2, int(sy) - 3, 1, 20, 14, 0);
        }
        SDL_RenderSetViewport(ren, nullptr);

        // Palette panel (left).
        fillRect(ren, 0, kMenuH, kPaletteW, canvasH, 30, 32, 40);
        SDL_Rect palClip{0, kMenuH, kPaletteW, canvasH};
        SDL_RenderSetClipRect(ren, &palClip);
        if (tool == UNITS) {   // a scrollable text list of unit types
            for (int i = 0; i < int(unitTypes.size()); ++i) {
                int ty = kMenuH + 2 + i * 14 - paletteScroll;
                if (ty + 12 < kMenuH || ty > h - kStatusH) continue;
                if (i == selectedType)
                    fillRect(ren, 0, ty - 1, kPaletteW, 13, 70, 66, 40);
                cart::drawText(ren, unitTypes[size_t(i)], 6, ty + 2, 1,
                               i == selectedType ? 255 : 200, i == selectedType ? 220 : 205,
                               i == selectedType ? 120 : 215);
            }
        } else {
        int palN = tool == FEATURES ? int(features.list().size())
                                    : int(sections.list().size());
        int palSel = tool == FEATURES ? selectedFeat : selected;
        for (int i = 0; i < palN; ++i) {
            int col = i % cols, row = i / cols;
            int cx = 4 + col * (kThumb + 4);
            int cy = kMenuH + 4 + row * (kThumb + 4) - paletteScroll;
            if (cy + kThumb < kMenuH || cy > h - kStatusH) continue;   // cull
            SDL_Rect cell{cx, cy, kThumb, kThumb};
            SDL_Texture* t = tool == FEATURES ? featTextureFor(features.list()[size_t(i)])
                                              : thumbFor(sections.list()[size_t(i)].path);
            fillRect(ren, cx, cy, kThumb, kThumb, 44, 46, 54);   // cell backing
            if (t) {
                if (tool == FEATURES) {   // feature sprite: fit-centre, keep aspect
                    const cart::FeatSprite* sp = features.sprite(vfs, features.list()[size_t(i)]);
                    float sc = std::min(float(kThumb) / std::max(sp->w, 1),
                                        float(kThumb) / std::max(sp->h, 1));
                    int dw = int(sp->w * sc), dh = int(sp->h * sc);
                    SDL_Rect fc{cx + (kThumb - dw) / 2, cy + (kThumb - dh) / 2, dw, dh};
                    SDL_RenderCopy(ren, t, nullptr, &fc);
                } else SDL_RenderCopy(ren, t, nullptr, &cell);
            }
            if (i == palSel) {   // selection highlight
                SDL_SetRenderDrawColor(ren, 255, 210, 90, 255);
                SDL_Rect b{cx - 1, cy - 1, kThumb + 2, kThumb + 2};
                SDL_RenderDrawRect(ren, &b);
                SDL_Rect b2{cx - 2, cy - 2, kThumb + 4, kThumb + 4};
                SDL_RenderDrawRect(ren, &b2);
            }
        }
        }   // end else (thumbnail palette)
        SDL_RenderSetClipRect(ren, nullptr);

        // Chrome strips.
        fillRect(ren, 0, 0, w, kMenuH, 46, 48, 58);
        fillRect(ren, 0, kMenuH - 1, w, 1, 12, 12, 16);
        fillRect(ren, kPaletteW - 1, kMenuH, 1, canvasH, 12, 12, 16);
        fillRect(ren, 0, h - kStatusH, w, kStatusH, 38, 40, 50);
        fillRect(ren, 0, h - kStatusH, w, 1, 12, 12, 16);

        // Menu strip: title + tool buttons (active one highlighted).
        cart::drawText(ren, "CARTOGRAPHER", 6, 7, 1, 200, 200, 210);
        const char* names[4] = {"TERRAIN", "FEATURES", "UNITS", "STARTS"};
        for (int t = 0; t < 4; ++t) {
            int bx = 96 + t * 72;
            bool active = int(tool) == t;
            fillRect(ren, bx, 3, 68, kMenuH - 6, active ? 90 : 60, active ? 80 : 62,
                     active ? 40 : 74);
            cart::drawText(ren, names[t], bx + 8, 7, 1, active ? 255 : 190,
                           active ? 220 : 190, active ? 120 : 200);
        }

        // Status bar: cursor cell, tool, zoom, start count.
        int mxg, myg; SDL_GetMouseState(&mxg, &myg); mxg /= kUIScale; myg /= kUIScale;
        int ccx, ccz;
        std::string coord = mouseCell(mxg, myg, ccx, ccz)
            ? "( " + std::to_string(ccx) + ", " + std::to_string(ccz) + " )" : "";
        char zbuf[16];
        std::snprintf(zbuf, sizeof zbuf, "%d%%", int(mapView.zoom() * 100 + 0.5f));
        std::string status = coord + "   TOOL: " + names[int(tool)] + "   ZOOM: " + zbuf;
        if (tool == UNITS)
            status += "   PLAYER: " + std::to_string(currentPlayer) +
                      "   UNITS: " + std::to_string(units.size());
        else
            status += "   STARTS: " + std::to_string(scenario.starts.size());
        if (!useOnly.empty()) status += "   USEONLY: " + std::to_string(useOnly.size());
        if (clearArm) status += "   CLEAR AREA: drag a box (K cancels)";
        cart::drawText(ren, status, 6, h - kStatusH + 7, 1, 200, 205, 215);

        // Clear Area drag box (screen-space rectangle).
        if (clearDrag) {
            SDL_Rect box{std::min(clx0, clx1), std::min(cly0, cly1),
                         std::abs(clx1 - clx0), std::abs(cly1 - cly0)};
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren, 230, 90, 80, 60); SDL_RenderFillRect(ren, &box);
            SDL_SetRenderDrawColor(ren, 240, 120, 100, 220); SDL_RenderDrawRect(ren, &box);
        }

        // Scripting (trigger) overlay: per-player rule groups + their
        // conditions and actions. Drawn under the picker / param modal.
        if (scriptOpen) {
            constexpr int kRow = 12;
            SDL_Rect ct = cart::drawPanel(ren, w, h, 780, 520, "SCENARIO SCRIPTING");
            auto& gs = scen.players[size_t(scrPlayer)];
            ta::crt::RuleGroup* g = curGroup();
            // Header row: player nav, rule count, Done.
            rPrevP = cart::drawButton(ren, ct.x, ct.y, 18, 14, "<", false);
            cart::drawText(ren, "PLAYER " + std::to_string(scrPlayer), ct.x + 24, ct.y + 3, 1, 220, 224, 235);
            rNextP = cart::drawButton(ren, ct.x + 104, ct.y, 18, 14, ">", false);
            cart::drawText(ren, std::to_string(gs.size()) + " RULES", ct.x + 132, ct.y + 3, 1, 175, 185, 200);
            rScrDone = cart::drawButton(ren, ct.x + ct.w - 60, ct.y, 56, 14, "DONE", true);

            int colW = (ct.w - 20) / 3;
            int x0 = ct.x, x1 = ct.x + colW + 10, x2 = ct.x + 2 * colW + 20;
            cart::drawText(ren, "RULES", x0, ct.y + 20, 1, 150, 200, 150);
            cart::drawText(ren, "CONDITIONS", x1, ct.y + 20, 1, 150, 200, 150);
            cart::drawText(ren, "ACTIONS", x2, ct.y + 20, 1, 150, 200, 150);
            int listY = ct.y + 32, listH = ct.h - 32 - 24;
            rRuleList = {x0, listY, colW, listH};
            rCondList = {x1, listY, colW, listH};
            rActList = {x2, listY, colW, listH};
            for (const SDL_Rect* a : {&rRuleList, &rCondList, &rActList}) {
                SDL_SetRenderDrawColor(ren, 22, 24, 32, 255); SDL_RenderFillRect(ren, a);
                SDL_SetRenderDrawColor(ren, 70, 74, 90, 255); SDL_RenderDrawRect(ren, a);
            }
            // Rule (group) list.
            SDL_RenderSetClipRect(ren, &rRuleList);
            for (int i = 0; i < int(gs.size()); ++i) {
                int ry = listY + i * kRow - scrRuleScroll;
                if (ry + kRow < listY || ry > listY + listH) continue;
                bool sel = i == scrGroup;
                if (sel) { SDL_SetRenderDrawColor(ren, 58, 68, 95, 255); SDL_Rect hr{x0, ry, colW, kRow}; SDL_RenderFillRect(ren, &hr); }
                std::string lbl = "Rule " + std::to_string(i + 1) + "  " +
                    std::to_string(gs[size_t(i)].conditions.size()) + "c/" +
                    std::to_string(gs[size_t(i)].actions.size()) + "a";
                cart::drawText(ren, lbl, x0 + 3, ry + 2, 1, sel ? 255 : 200, sel ? 235 : 205, sel ? 200 : 215);
            }
            SDL_RenderSetClipRect(ren, nullptr);
            // Conditions + actions of the selected group.
            auto drawRules = [&](const SDL_Rect& area, int scroll, const std::vector<ta::crt::Rule>* rules,
                                 bool isAct, int selRow) {
                if (!rules) return;
                SDL_RenderSetClipRect(ren, &area);
                for (int i = 0; i < int(rules->size()); ++i) {
                    int ry = area.y + i * kRow - scroll;
                    if (ry + kRow < area.y || ry > area.y + area.h) continue;
                    bool sel = i == selRow;
                    if (sel) { SDL_SetRenderDrawColor(ren, 58, 68, 95, 255); SDL_Rect hr{area.x, ry, area.w, kRow}; SDL_RenderFillRect(ren, &hr); }
                    cart::drawText(ren, cart::formatRule(isAct, (*rules)[size_t(i)]), area.x + 3, ry + 2, 1,
                                   sel ? 255 : 205, sel ? 235 : 210, sel ? 200 : 220);
                }
                SDL_RenderSetClipRect(ren, nullptr);
            };
            drawRules(rCondList, scrCondScroll, g ? &g->conditions : nullptr, false, scrCondSel);
            drawRules(rActList, scrActScroll, g ? &g->actions : nullptr, true, scrActSel);
            // Column action buttons.
            int by = ct.y + ct.h - 18;
            rAddRule = cart::drawButton(ren, x0, by, 44, 16, "+RULE", true);
            rDelRule = cart::drawButton(ren, x0 + 48, by, 44, 16, "-RULE", false);
            rAddCond = cart::drawButton(ren, x1, by, 44, 16, "+COND", true);
            rDelCond = cart::drawButton(ren, x1 + 48, by, 44, 16, "-COND", false);
            rAddAct = cart::drawButton(ren, x2, by, 40, 16, "+ACT", true);
            rDelAct = cart::drawButton(ren, x2 + 44, by, 40, 16, "-ACT", false);
            cart::drawText(ren, "double-click a condition/action to edit its parameters",
                           ct.x, by - 12, 1, 150, 154, 168);

            // Nested opcode picker (choose a condition/action type to add).
            if (pickOpen) {
                const auto& defs = pickAction ? cart::actionDefs() : cart::conditionDefs();
                SDL_Rect pc = cart::drawPanel(ren, w, h, 480, 400, pickAction ? "ADD ACTION" : "ADD CONDITION");
                rPickList = {pc.x, pc.y, pc.w, pc.h - 28};
                SDL_RenderSetClipRect(ren, &rPickList);
                for (int i = 0; i < int(defs.size()); ++i) {
                    int ry = pc.y + i * kRow - pickScroll;
                    if (ry + kRow < pc.y || ry > pc.y + rPickList.h) continue;
                    cart::drawText(ren, defs[size_t(i)].templ, pc.x + 3, ry + 2, 1, 210, 214, 225);
                }
                SDL_RenderSetClipRect(ren, nullptr);
                rPickCancel = cart::drawButton(ren, pc.x + pc.w - 74, pc.y + pc.h - 20, 70, 18, "CANCEL", false);
            }
        }

        // Save-before-exit prompt: SAVE / DON'T SAVE / CANCEL.
        if (modal == M_QUITSAVE) {
            SDL_Rect ct = cart::drawPanel(ren, w, h, 380, 92, "UNSAVED CHANGES");
            cart::drawText(ren, "Save changes before exiting?", ct.x, ct.y, 1, 225, 228, 236);
            mOK = cart::drawButton(ren, ct.x, ct.y + ct.h - 20, 60, 18, "SAVE", true);
            mQuit = cart::drawButton(ren, ct.x + 66, ct.y + ct.h - 20, 78, 18, "DISCARD", false);
            mCancel = cart::drawButton(ren, ct.x + ct.w - 74, ct.y + ct.h - 20, 70, 18, "CANCEL", false);
        }
        // Message box (Check Map result) / confirmation: wrapped text + OK
        // (message) or OK + CANCEL (confirm).
        else if (modal == M_MESSAGE || modal == M_CONFIRM) {
            int ph = 66 + int(mMsg.size()) * 12;
            SDL_Rect ct = cart::drawPanel(ren, w, h, 360, ph, mTitle);
            for (size_t i = 0; i < mMsg.size(); ++i)
                cart::drawText(ren, mMsg[i], ct.x, ct.y + int(i) * 12, 1, 225, 228, 236);
            if (modal == M_CONFIRM) {
                mOK = cart::drawButton(ren, ct.x + ct.w - 150, ct.y + ct.h - 20, 70, 18, "OK", true);
                mCancel = cart::drawButton(ren, ct.x + ct.w - 74, ct.y + ct.h - 20, 70, 18, "CANCEL", false);
            } else {
                mOK = cart::drawButton(ren, ct.x + ct.w - 74, ct.y + ct.h - 20, 70, 18, "OK", true);
                mCancel = {};   // no cancel on a message box
            }
        } else if (modal != M_NONE) {
            // N-field dialog; height fits the field count.
            int ph = 70 + mN * 40;
            SDL_Rect ct = cart::drawPanel(ren, w, h, 320, ph, mTitle);
            if (modal == M_UNIT && editUnit >= 0 && editUnit < int(units.size()))
                cart::drawText(ren, units[size_t(editUnit)].type, ct.x, ct.y - 16, 1, 200, 200, 200);
            for (int i = 0; i < mN; ++i) {
                if (mfChoices[i])
                    mBox[i] = cart::drawChoice(ren, ct.x, ct.y + i * 40, ct.w, mLabel[i],
                                               mf[i], mDropOpen == i);
                else
                    mBox[i] = cart::drawField(ren, ct.x, ct.y + i * 40, ct.w, mLabel[i],
                                              mf[i], mfocus == i);
            }
            // New Map gets a RANDOM shortcut (procedural terrain, jump straight in).
            if (modal == M_NEW) {
                mOK = cart::drawButton(ren, ct.x, ct.y + ct.h - 20, 74, 18, "CREATE", true);
                mRandom = cart::drawButton(ren, ct.x + 80, ct.y + ct.h - 20, 74, 18, "RANDOM", false);
            } else {
                mOK = cart::drawButton(ren, ct.x + ct.w - 150, ct.y + ct.h - 20, 70, 18, "OK", true);
                mRandom = {};
            }
            mCancel = cart::drawButton(ren, ct.x + ct.w - 74, ct.y + ct.h - 20, 70, 18,
                                       "CANCEL", false);
            // Open dropdown list, drawn LAST so it overlays the fields below it.
            mDropRects.clear();
            if (mDropOpen >= 0 && mDropOpen < mN && mfChoices[mDropOpen]) {
                const auto& opts = *mfChoices[mDropOpen];
                const SDL_Rect& anchor = mBox[mDropOpen];
                constexpr int kRowH = 14;
                SDL_Rect list{anchor.x, anchor.y + anchor.h, anchor.w,
                              kRowH * int(opts.size()) + 2};
                SDL_SetRenderDrawColor(ren, 28, 30, 40, 255); SDL_RenderFillRect(ren, &list);
                SDL_SetRenderDrawColor(ren, 150, 200, 120, 255); SDL_RenderDrawRect(ren, &list);
                for (size_t i = 0; i < opts.size(); ++i) {
                    SDL_Rect r{list.x + 1, list.y + 1 + int(i) * kRowH, list.w - 2, kRowH};
                    if (opts[i] == mf[mDropOpen]) {
                        SDL_SetRenderDrawColor(ren, 60, 80, 50, 255); SDL_RenderFillRect(ren, &r);
                    }
                    cart::drawText(ren, opts[i], r.x + 4, r.y + 3, 1, 225, 230, 240);
                    mDropRects.push_back(r);
                }
            }
        }

        // Use Only checklist overlay: scrollable unit-type list, click to toggle.
        if (useOnlyOpen) {
            SDL_Rect ct = cart::drawPanel(ren, w, h, 320, 460, "USE ONLY UNITS");
            std::string hdr = useOnly.empty()
                ? "ALL UNITS ALLOWED (empty = no restriction)"
                : std::to_string(useOnly.size()) + " ALLOWED  (click to toggle)";
            cart::drawText(ren, hdr, ct.x, ct.y - 16, 1, 180, 205, 185);
            int listH = ct.h - 30;
            uoList = {ct.x, ct.y, ct.w, listH};
            SDL_RenderSetClipRect(ren, &uoList);
            for (int i = 0; i < int(unitTypes.size()); ++i) {
                int ry = ct.y + i * 14 - useOnlyScroll;
                if (ry + 12 < ct.y || ry > ct.y + listH) continue;   // cull
                bool on = useOnly.count(unitTypes[size_t(i)]) > 0;
                cart::drawText(ren, (on ? "[X] " : "[ ] ") + unitTypes[size_t(i)],
                               ct.x + 2, ry, 1, on ? 235 : 128, on ? 235 : 130, on ? 180 : 138);
            }
            SDL_RenderSetClipRect(ren, nullptr);
            uoClear = cart::drawButton(ren, ct.x, ct.y + ct.h - 20, 100, 18, "UNRESTRICT", false);
            uoDone = cart::drawButton(ren, ct.x + ct.w - 74, ct.y + ct.h - 20, 70, 18, "DONE", true);
        }

        SDL_RenderPresent(ren);
        if (!shotPath.empty()) {
            int ow, oh; SDL_GetRendererOutputSize(ren, &ow, &oh);
            std::vector<uint8_t> px(size_t(ow) * oh * 4);
            if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_RGBA32,
                                     px.data(), ow * 4) == 0) {
                ta::png::write(shotPath, ow, oh, px);
                std::fprintf(stderr, "shot %s (%dx%d)\n", shotPath.c_str(), ow, oh);
            }
            running = false;
        }
    }

    for (auto& [k, t] : thumbs) if (t) SDL_DestroyTexture(t);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
