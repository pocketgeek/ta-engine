#include "client/tafrontend.h"

#include "client/taguiview.h"
#include "gaf/gaf.h"
#include "gui/tagui.h"
#include "hpi/hpi.h"
#include "sim/sides.h"
#include "tdf/tdf.h"
#include "tnt/tnt.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>

namespace ta {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The skirmish map list: every installed map, sorted by name.
std::vector<std::pair<std::string, std::string>> skirmishMaps(const hpi::Vfs& vfs) {
    auto maps = hpi::listMaps(vfs);
    std::sort(maps.begin(), maps.end(), [](const auto& a, const auto& b) { return upper(a.first) < upper(b.first); });
    return maps;
}

struct Pic { SDL_Texture* tex = nullptr; int w = 0, h = 0; };

Pic minimapPicture(SDL_Renderer* ren, const hpi::Vfs& vfs, const std::string& path) {
    Pic p;
    try {
        const auto map = tnt::Map::load(vfs.read(path), path);
        if (map.minimapW <= 0 || map.minimapH <= 0) return p;
        const auto pal = gaf::Palette::fromBytes(vfs.read("palettes/palette.pal"), "palette.pal");
        std::vector<uint8_t> rgba(size_t(map.minimapW) * size_t(map.minimapH) * 4);
        int usedW = 0, usedH = 0;   // TA pads minimaps with index 0x64 (255,0..): trim the pad
        for (int y = 0; y < map.minimapH; ++y)
            for (int x = 0; x < map.minimapW; ++x) {
                const uint8_t idx = map.minimap[size_t(y) * size_t(map.minimapW) + size_t(x)];
                uint8_t* o = &rgba[(size_t(y) * size_t(map.minimapW) + size_t(x)) * 4];
                o[0] = pal.rgba[idx][0]; o[1] = pal.rgba[idx][1]; o[2] = pal.rgba[idx][2]; o[3] = 255;
                if (idx != 0x64) { usedW = std::max(usedW, x + 1); usedH = std::max(usedH, y + 1); }
            }
        p.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, map.minimapW, map.minimapH);
        SDL_UpdateTexture(p.tex, nullptr, rgba.data(), map.minimapW * 4);
        p.w = usedW ? usedW : map.minimapW;
        p.h = usedH ? usedH : map.minimapH;
    } catch (...) {}
    return p;
}

std::string mapDescription(const hpi::Vfs& vfs, const std::string& tntPath, std::string* size) {
    try {
        std::string ota = tntPath.substr(0, tntPath.size() - 4) + ".ota";
        auto b = vfs.read(ota);
        auto root = tdf::parseText(std::string(b.begin(), b.end()), ota);
        if (const auto* gh = root.child("GlobalHeader")) {
            if (size) *size = gh->valueOr("size", "");
            return gh->valueOr("missiondescription", "");
        }
    } catch (...) {}
    return {};
}

}  // namespace

SkirmishSetup defaultSkirmish(const hpi::Vfs& vfs) {
    SkirmishSetup s;
    const auto maps = skirmishMaps(vfs);
    if (!maps.empty()) s.map = maps.front().first;     // SkirmishMap: the first in the list
    const int sides = std::max(1, sides::count());
    for (int n = 0; n < 10; ++n) {
        SkirmishSeat seat;
        seat.side = n % sides;     // Player<n>Side = n & 1: alternating Arm/Core
        seat.color = n;
        seat.controller = n == 0 ? 1 : n == 1 ? 2 : 0;   // a first game: you against one computer
        s.seats.push_back(seat);
    }
    return s;
}

TaFrontEnd::TaFrontEnd(SDL_Renderer* ren, SDL_Window* win, const hpi::Vfs& vfs) : ren_(ren), win_(win), vfs_(vfs) {}

TaFrontEnd::Choice TaFrontEnd::run(SkirmishSetup& setup) {
    SDL_StartTextInput();
    if (setup.seats.empty()) setup = defaultSkirmish(vfs_);
    std::unique_ptr<TaGuiView> view, popup;
    std::unique_ptr<TaGuiView> skirmArt;   // skirmish.gaf rows
    auto open = [&](const char* screen, const char* backdrop) {
        view = std::make_unique<TaGuiView>(ren_, vfs_, tagui::load(vfs_, screen), true);
        if (backdrop) view->setBackdrop(std::string("bitmaps/") + backdrop + ".pcx");
    };
    const auto maps = skirmishMaps(vfs_);
    Pic preview;
    std::string previewFor;
    auto syncSkirmish = [&]() {
        if (!view) return;
        view->setStage("CommanderDeath", setup.commanderEnds ? 0 : 1);
        view->setStage("StartLocation", setup.fixedStarts ? 0 : 1);
        view->setStage("Mapping", setup.mapped ? 1 : 0);
        view->setStage("LineOfSight", setup.lineOfSight);
        view->setStage("Difficulty", setup.difficulty);
        view->setText("MapName", setup.map);
    };
    auto readSkirmish = [&]() {
        setup.commanderEnds = view->stage("CommanderDeath") == 0;
        setup.fixedStarts = view->stage("StartLocation") == 0;
        setup.mapped = view->stage("Mapping") == 1;
        setup.lineOfSight = view->stage("LineOfSight");
        setup.difficulty = view->stage("Difficulty");
    };
    auto goPage = [&](Page p) {
        page_ = p;
        popup.reset();
        switch (p) {
        case Page::Main: open("mainmenu", "frontendx"); break;
        case Page::Single: open("single", "singlebg"); break;
        case Page::Skirmish: open("skirmish", "skirmsetup4x"); syncSkirmish(); break;
        case Page::SelectMap: break;
        case Page::SelProv:
            open("selprov", "selconnect2");
            view->setList("DPLAY", {"TA Engine server (TCP/IP)"});
            view->setSelection("DPLAY", 0);
            view->setVisible("SERVICEX", false);
            break;
        }
    };
    goPage(Page::Main);
    // Player rows (code-drawn on SKIRMISH with skirmish.gaf).
    constexpr int kRowX = 46, kRowY = 79, kRowH = 20, kRows = 10;
    struct Col { int x, w; };
    constexpr Col kName{kRowX, 112}, kSide{kRowX + 116, 44}, kColor{kRowX + 162, 38},
                  kAlly{kRowX + 202, 38}, kMetal{kRowX + 242, 46}, kEnergy{kRowX + 292, 46};
    tagui::Screen artScreen;   // a carrier for skirmish.gaf lookups
    artScreen.name = "SKIRMISH";
    artScreen.gadgets.push_back(tagui::Gadget{});
    TaGuiView art(ren_, vfs_, artScreen, true);
    art.setBackdrop("bitmaps/skirmsetup4x.pcx");   // the rows share the page palette
    TaFont rowFont;
    rowFont.loadGaf(ren_, vfs_, "hattfont12", gaf::Palette::fromBytes(vfs_.read("bitmaps/skirmsetup4x.pcx"), "skirmsetup4x.pcx"));
    static const char* kControllers[3] = {"Open", "Player", "Computer"};
    auto cycleValue = [](int v) {   // metal/energy start: 1000 .. 10000 [U: retail steps not traced]
        static const int steps[] = {1000, 2500, 5000, 10000};
        for (size_t i = 0; i < 4; ++i) if (steps[i] == v) return steps[(i + 1) % 4];
        return 1000;
    };
    std::string message;
    for (;;) {
        int w = 640, h = 480;
        SDL_GetRendererOutputSize(ren_, &w, &h);
        const GuiLayout L(w, h);
        SDL_Event e;
        auto onHit = [&](const std::string& h2) -> std::optional<Choice> {
            if (popup && page_ == Page::SelProv) {   // TCP address
                if (h2 == "PREV") popup.reset();
                else if (h2 == "OK") {
                    const auto* f = popup->screen().find("ADDRESS");
                    server_ = f ? f->text : std::string();
                    return Choice::Multiplayer;
                }
                return std::nullopt;
            }
            if (popup) {   // SELMAP
            if (h2 == "PREVMENU") popup.reset();
            else if (h2 == "LOAD" || h2 == "MAPNAMES") {
                const int sel = popup->selection("MAPNAMES");
                if (sel >= 0 && sel < int(maps.size())) setup.map = maps[size_t(sel)].first;
                if (h2 == "LOAD") { popup.reset(); syncSkirmish(); }
            }
            return std::nullopt;
        }
        switch (page_) {
        case Page::Main:
            if (h2 == "SINGLE") goPage(Page::Single);
            else if (h2 == "MULTI") goPage(Page::SelProv);
            else if (h2 == "EXIT") return Choice::Exit;
            break;
        case Page::Single:
            if (h2 == "SKIRMISH") goPage(Page::Skirmish);
            else if (h2 == "NEWCAMP") return Choice::Campaign;
            else if (h2 == "OPTIONS") return Choice::Options;
            else if (h2 == "PREVMENU") goPage(Page::Main);
            break;
        case Page::Skirmish:
            readSkirmish();
            if (h2 == "PREVMENU") goPage(Page::Single);
            else if (h2 == "SELECTMAP") {
                popup = std::make_unique<TaGuiView>(ren_, vfs_, tagui::load(vfs_, "selmap"), true);
                popup->setBackdrop("bitmaps/dselectmap2.pcx");
                std::vector<std::string> names;
                int sel = 0;
                for (size_t i = 0; i < maps.size(); ++i) {
                    names.push_back(maps[i].first);
                    if (upper(maps[i].first) == upper(setup.map)) sel = int(i);
                }
                popup->setList("MAPNAMES", names);
                popup->setSelection("MAPNAMES", sel);
            } else if (h2 == "START") {
                // Retail's checks (0x47ae60): a map, at least one Player and one
                // Computer, and not everyone in one ally group.
                int humans = 0, cpus = 0;
                std::vector<int> allies;
                for (const auto& s : setup.seats) {
                    if (s.controller == 1) ++humans;
                    if (s.controller == 2) ++cpus;
                    if (s.controller) allies.push_back(s.ally);
                }
                const bool oneTeam = allies.size() > 1 && allies[0] != 5 &&
                                     std::all_of(allies.begin(), allies.end(), [&](int a) { return a == allies[0]; });
                if (setup.map.empty()) message = "Select a map first.";
                else if (humans != 1 || cpus < 1) message = "A skirmish needs one Player and at least one Computer.";
                else if (oneTeam) message = "Everyone is on the same team.";
                else return Choice::Skirmish;
            }
            break;
        case Page::SelProv:
            if (h2 == "PREVMENU") goPage(Page::Main);
            else if (h2 == "SELECT" || h2 == "DPLAY") {
                popup = std::make_unique<TaGuiView>(ren_, vfs_, tagui::load(vfs_, "tcp"), true);
                popup->setText("ADDRESS", server_);
            }
            break;
        default: break;
        }
            return std::nullopt;
        };
#ifndef NDEBUG
        // TA_FRONT_SCRIPT=GADGET,GADGET,...: press each in turn, one per frame after the
        // first draws (headless front-end runs); TA_FRONT_SHOT=<prefix>: save each page.
        static std::vector<std::string> script = [] {
            std::vector<std::string> v;
            if (const char* e = std::getenv("TA_FRONT_SCRIPT")) {
                std::string cur;
                for (const char* p = e;; ++p) {
                    if (*p == ',' || !*p) { if (!cur.empty()) v.push_back(cur); cur.clear(); if (!*p) break; }
                    else cur.push_back(*p);
                }
            }
            return v;
        }();
        static int frame = 0;
        if (++frame > 3 && !script.empty()) {
            const std::string name = script.front();
            script.erase(script.begin());
            TaGuiView& active = popup ? *popup : *view;
            const std::string got = active.activate(name);
            std::fprintf(stderr, "front-end script: %s -> %s\n", name.c_str(), got.empty() ? "(none)" : got.c_str());
            if (!got.empty()) { const auto r = onHit(upper(got)); if (r) return *r; }
        }
#endif
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return Choice::Exit;
            TaGuiView& active = popup ? *popup : *view;
            const std::string hit = active.handle(e, L);
            if (page_ == Page::Skirmish && !popup && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                const float mx = (float(e.button.x) - L.ox) / L.scale, my = (float(e.button.y) - L.oy) / L.scale;
                const int row = int((my - kRowY) / kRowH);
                if (my >= kRowY && row >= 0 && row < kRows && row < int(setup.seats.size())) {
                    auto& seat = setup.seats[size_t(row)];
                    auto in = [&](const Col& c) { return mx >= float(c.x) && mx < float(c.x + c.w); };
                    if (in(kName)) {
                        int next = (seat.controller + 1) % 3;
                        if (next == 1)   // only one human in a skirmish
                            for (const auto& s : setup.seats) if (s.controller == 1) { next = 2; break; }
                        seat.controller = next;
                    } else if (seat.controller != 0) {
                        if (in(kSide)) seat.side = (seat.side + 1) % std::max(1, sides::count());
                        else if (in(kColor)) seat.color = (seat.color + 1) % 10;
                        else if (in(kAlly)) seat.ally = (seat.ally + 1) % 6;
                        else if (in(kMetal)) seat.metal = cycleValue(seat.metal);
                        else if (in(kEnergy)) seat.energy = cycleValue(seat.energy);
                    }
                }
            }
            if (hit.empty()) continue;
            {
            const auto r = onHit(upper(hit));
            if (r) return *r;
            }
            continue;
        }
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
        SDL_RenderClear(ren_);
        if (view) view->draw(L);
        if (page_ == Page::Skirmish && view) {
            for (int r = 0; r < kRows && r < int(setup.seats.size()); ++r) {
                const auto& seat = setup.seats[size_t(r)];
                const float y = float(kRowY + r * kRowH);
                SDL_Color white{255, 255, 255, 255};
                rowFont.draw(ren_, L, float(kName.x + 6), y + 4, kControllers[std::clamp(seat.controller, 0, 2)], white);
                if (seat.controller) {
                    rowFont.draw(ren_, L, float(kSide.x + 4), y + 4, sides::at(seat.side).name, white);
                    const SDL_FRect sw = L.rect(float(kColor.x + 8), y + 4, 20, 12);
                    static const SDL_Color kTeam[10] = {{0, 64, 255, 255}, {255, 0, 0, 255}, {255, 255, 255, 255},
                        {0, 192, 0, 255}, {0, 0, 128, 255}, {128, 0, 255, 255}, {255, 255, 0, 255},
                        {32, 32, 32, 255}, {128, 192, 255, 255}, {192, 160, 128, 255}};
                    const SDL_Color tc = kTeam[seat.color % 10];
                    SDL_SetRenderDrawColor(ren_, tc.r, tc.g, tc.b, 255);
                    SDL_RenderFillRectF(ren_, &sw);
                    rowFont.draw(ren_, L, float(kAlly.x + 10), y + 4, seat.ally >= 5 ? "-" : std::to_string(seat.ally + 1), white);
                    rowFont.draw(ren_, L, float(kMetal.x + 4), y + 4, std::to_string(seat.metal), white);
                    rowFont.draw(ren_, L, float(kEnergy.x + 4), y + 4, std::to_string(seat.energy), white);
                }
            }
            if (!message.empty()) rowFont.draw(ren_, L, 46, 335, message, SDL_Color{255, 220, 120, 255});
        }
        if (popup) {
            const int sel = popup->selection("MAPNAMES");
            const std::string want = sel >= 0 && sel < int(maps.size()) ? maps[size_t(sel)].second : "";
            if (want != previewFor) {
                if (preview.tex) SDL_DestroyTexture(preview.tex);
                preview = minimapPicture(ren_, vfs_, want);
                previewFor = want;
                std::string size;
                popup->setText("DESCRIPTION", mapDescription(vfs_, want, &size));
                popup->setText("SIZE", size);
            }
            popup->drawSurface = [&](const tagui::Gadget& g, SDL_FRect r) {
                if (upper(g.name) != "MAPPIC" || !preview.tex) return;
                SDL_Rect src{0, 0, preview.w, preview.h};
                const float s = std::min(r.w / float(preview.w), r.h / float(preview.h));
                SDL_FRect dst{r.x + (r.w - preview.w * s) / 2, r.y + (r.h - preview.h * s) / 2, preview.w * s, preview.h * s};
                SDL_RenderCopyF(ren_, preview.tex, &src, &dst);
            };
            popup->draw(L);
        }
        SDL_RenderPresent(ren_);
        SDL_Delay(10);
    }
}

}  // namespace ta
