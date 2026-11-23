#include "client/tafrontend.h"

#include "client/taguiview.h"
#include "gaf/gaf.h"
#include "gui/tagui.h"
#include "hpi/hpi.h"
#include "sim/sides.h"
#include "tdf/tdf.h"
#include "tnt/tnt.h"
#include "util/png.h"
#include "util/virtualpath.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>

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

// ---- campaigns (docs/retail-campaign.md) -----------------------------------

namespace {
// A briefing narration or glamour sound (camps/briefs/*.wav): decoded by SDL and
// played on its own queue-driven device, so the front end needs no sound bank.
class WavClip {
public:
    ~WavClip() { stop(); }
    bool play(const hpi::Vfs& vfs, const std::string& path) {
        stop();
        std::vector<uint8_t> bytes;
        try { bytes = vfs.read(path); } catch (...) { return false; }
        if (bytes.empty() || SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
        SDL_AudioSpec spec{};
        Uint8* buf = nullptr; Uint32 len = 0;
        if (!SDL_LoadWAV_RW(SDL_RWFromConstMem(bytes.data(), int(bytes.size())), 1, &spec, &buf, &len)) return false;
        spec.callback = nullptr;
        dev_ = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
        if (dev_) { SDL_QueueAudio(dev_, buf, len); SDL_PauseAudioDevice(dev_, 0); }
        SDL_FreeWAV(buf);
        return dev_ != 0;
    }
    void stop() {
        if (dev_) { SDL_ClearQueuedAudio(dev_); SDL_CloseAudioDevice(dev_); dev_ = 0; }
    }
    bool playing() const { return dev_ && SDL_GetQueuedAudioSize(dev_) > 0; }
private:
    SDL_AudioDeviceID dev_ = 0;
};
}  // namespace

void TaFrontEnd::showGlamour(const std::string& glamour, const std::string& sound) {
    // 0x41db1b: a won mission's victory picture, bitmaps/glamour/<glamour>.pcx,
    // with camps/briefs/<glamoursound>.wav; any key or click goes on.
    const std::string pcx = "bitmaps/glamour/" + glamour + ".pcx";
    if (glamour.empty() || !vfs_.has(pcx)) return;
    SDL_Texture* tex = nullptr;
    int tw = 0, th = 0;
    try {
        const auto img = gaf::loadPcx(vfs_.read(pcx), pcx);
        tw = img.width; th = img.height;
        tex = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, tw, th);
        if (tex) SDL_UpdateTexture(tex, nullptr, img.rgba.data(), tw * 4);
    } catch (...) {}
    if (!tex) return;
    WavClip clip;
    if (!sound.empty()) clip.play(vfs_, "camps/briefs/" + sound + ".wav");
    const bool headless = [] { const char* d = SDL_GetCurrentVideoDriver(); return d && std::string(d) == "dummy"; }();
    const Uint32 until = SDL_GetTicks() + (headless ? 0u : 60000u);
    for (bool done = false; !done;) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT || e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN) done = true;
        int w = 640, h = 480;
        SDL_GetRendererOutputSize(ren_, &w, &h);
        const GuiLayout L(w, h);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
        SDL_RenderClear(ren_);
        const SDL_FRect dst = L.rect(0, 0, 640, 480);
        SDL_RenderCopyF(ren_, tex, nullptr, &dst);
#ifndef NDEBUG
        if (const char* shot = std::getenv("TA_GLAMOUR_SHOT")) {
            std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
            if (SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_ABGR8888, px.data(), w * 4) == 0)
                png::write(shot, w, h, px);
        }
#endif
        SDL_RenderPresent(ren_);
        if (SDL_GetTicks() >= until) done = true;
        SDL_Delay(15);
    }
    SDL_DestroyTexture(tex);
}

std::string TaCampaign::spec() const {
    return active() ? missions[size_t(std::clamp(cur, 0, int(missions.size()) - 1))].file + ":" +
                          std::to_string(difficulty)
                    : std::string();
}

std::string TaCampaign::mapName() const {
    std::string f = active() ? missions[size_t(std::clamp(cur, 0, int(missions.size()) - 1))].file : "";
    if (f.size() > 4 && upper(f.substr(f.size() - 4)) == ".OTA") f.resize(f.size() - 4);
    return f;
}

namespace {
std::optional<tdf::Node> readTdf(const hpi::Vfs& vfs, const std::string& path) {
    if (!vfs.has(path)) return std::nullopt;
    try {
        const auto b = vfs.read(path);
        return tdf::parseText(std::string(b.begin(), b.end()), path);
    } catch (...) { return std::nullopt; }
}
std::string stemOf(const std::string& path) {
    std::string s = path.substr(path.find_last_of('/') + 1);
    if (const auto dot = s.rfind('.'); dot != std::string::npos) s.resize(dot);
    return s;
}
}  // namespace

std::vector<std::string> taCampaignList(const hpi::Vfs& vfs, const std::string& sideName) {
    std::vector<std::string> out;
    for (const auto& p : vfs.list("camps")) {
        if (vpath::extension(p) != ".tdf" || p.find('/', 6) != std::string::npos) continue;
        const auto root = readTdf(vfs, p);
        const tdf::Node* h = root ? root->child("HEADER") : nullptr;
        if (!h) continue;   // no [HEADER]: skipped
        const std::string side = h->valueOr("campaignside", "");
        if (side == sideName || side == "ALL") out.push_back(stemOf(p));
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return upper(a) < upper(b); });
    return out;
}

bool loadTaCampaign(const hpi::Vfs& vfs, const std::string& name, TaCampaign& out) {
    const auto root = readTdf(vfs, "camps/" + name + ".tdf");
    if (!root) return false;
    out.name = name;
    out.missions.clear();
    for (int i = 0;; ++i) {
        const tdf::Node* m = root->child("MISSION" + std::to_string(i));
        if (!m) break;   // the first gap ends the list (0x435760)
        TaCampaign::Mission e;
        e.file = m->valueOr("missionfile", "");
        e.name = m->valueOr("missionname", "Error -- Unnamed Mission");
        out.missions.push_back(e);
    }
    return !out.missions.empty();
}

void TaFrontEnd::setFailure(int failTo, std::string message) {
    // FailTo (sessionfailure.h): 2 ServerSelect, 5 CampaignPicker; the rest open MAINMENU.
    start_ = failTo == 2 ? Page::SelProv : failTo == 5 ? Page::NewGame : Page::Main;
    failure_ = std::move(message);
}

TaFrontEnd::Choice TaFrontEnd::endMission(SkirmishSetup& setup, TaCampaign& camp, int outcome) {
    // 0x41dc20: results[cur] = W / L.
    if (camp.cur >= 0 && camp.cur < int(camp.results.size())) camp.results[size_t(camp.cur)] = outcome > 0 ? 'W' : 'L';
    outcome_ = outcome;
    start_ = Page::EndMission;
    return run(setup, camp);
}

TaFrontEnd::Choice TaFrontEnd::run(SkirmishSetup& setup, TaCampaign& camp) {
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
    // ---- campaign screen state ----
    std::vector<std::string> campNames;    // NEWGAME's Campaign list for the side
    TaCampaign browse = camp;              // the campaign highlighted on NEWGAME
    struct BriefLine { std::string text; SDL_Color color; };
    std::vector<std::vector<BriefLine>> briefLines;   // wrapped, coloured runs per line
    int briefScroll = 0;
    std::unique_ptr<TaFont> briefFont;
    std::string briefGaf;                  // anims/<planet>brief.gaf
    WavClip narration;                     // camps/briefs/<narration>.wav (SHUTUP mutes)
    std::string narrationWav;
    std::string seqRotate, seqPan, seqSolar;
    int briefFrames[3] = {1, 1, 1};
    auto fillMissions = [&](TaGuiView& v, const TaCampaign& c, bool marks) {
        std::vector<std::string> rows;
        for (size_t i = 0; i < c.missions.size(); ++i) {
            // ENDMSN marks each mission won / lost / unplayed (retail draws glyphs;
            // the engine writes the mark).
            const char r = i < c.results.size() ? c.results[i] : 'U';
            rows.push_back(marks ? std::string(r == 'W' ? "Won   " : r == 'L' ? "Lost  " : "      ") + c.missions[i].name
                                 : c.missions[i].name);
        }
        v.setList("Missions", rows);
    };
    auto syncNewGame = [&]() {
        view->setPressed("Arm", camp.side == 0);
        view->setPressed("Core", camp.side == 1);
        view->setText("SIDENAME", sides::count() ? sides::at(camp.side).name : "");
        campNames = taCampaignList(vfs_, sides::count() ? sides::at(camp.side).name : "");
        view->setList("Campaign", campNames);
        int sel = 0;
        for (size_t i = 0; i < campNames.size(); ++i) if (campNames[i] == browse.name) sel = int(i);
        view->setSelection("Campaign", campNames.empty() ? -1 : sel);
        if (!campNames.empty() && browse.name != campNames[size_t(sel)]) loadTaCampaign(vfs_, campNames[size_t(sel)], browse);
        // The Missions list shows only in retail's any-mission mode (DRDEATH); a
        // campaign starts at mission 0, and ENDMSN lets the player pick any.
        view->setVisible("Missions", false);
        view->setVisible("MissionsKnob", false);
        view->setStage("Difficulty", camp.difficulty);
    };
    // MSNBRIEF: the mission's GlobalHeader brief / planet (docs/retail-campaign.md 4.2).
    auto openBrief = [&]() {
        briefLines.clear(); briefScroll = 0;
        std::string brief, planet;
        narrationWav.clear();
        if (camp.active())
            if (const auto ota = readTdf(vfs_, "maps/" + camp.missions[size_t(camp.cur)].file))
                if (const tdf::Node* gh = ota->child("GlobalHeader")) {
                    brief = gh->valueOr("brief", "");
                    planet = gh->valueOr("planet", "");
                    if (const auto n = gh->valueOr("narration", ""); !n.empty())
                        narrationWav = "camps/briefs/" + n + ".wav";
                }
        // The narration plays on entry (0x478dc7) unless SHUTUP is set.
        if (!narrationWav.empty() && view->stage("SHUTUP") == 0) narration.play(vfs_, narrationWav);
        const std::string pcx = "bitmaps/mbrief" + sides::lowerPrefix(camp.side) + ".pcx";
        view->setBackdrop(pcx);
        briefFont = std::make_unique<TaFont>();
        try { briefFont->loadGaf(ren_, vfs_, "hattfont12", gaf::Palette::fromBytes(vfs_.read(pcx), pcx)); } catch (...) {}
        // 0x4790dd: Planet -> anims/<x>brief.gaf, default Greenbrief.
        static const char* kPlanets[][2] = {{"Green planet", "green"}, {"Archipelago", "archi"},
            {"Wet Desert", "wdesert"}, {"Desert", "desert"}, {"Lava", "lava"}, {"Red Planet", "mars"},
            {"Lunar", "lunar"}, {"Metal", "metal"}, {"Lunar2", "lunar2"}, {"Ice", "ice"}, {"Lush", "lush"},
            {"Slate", "slate"}, {"Water World", "water"}, {"Acid", "acid"}, {"Crystal", "crystal"}};
        std::string key = "green";
        for (const auto& p : kPlanets) if (upper(planet) == upper(p[0])) key = p[1];
        briefGaf = key + "brief";   // anims/<key>brief.gaf
        seqRotate.clear(); seqPan.clear(); seqSolar.clear();
        try {
            const std::string gafPath = "anims/" + briefGaf + ".gaf";
            const auto seqs = gaf::load(vfs_.read(gafPath), gaf::Palette::fromBytes(vfs_.read(pcx), pcx), -1, gafPath);
            for (const auto& s : seqs) {
                const std::string n = upper(s.name);
                auto ends = [&](const char* suf) { const std::string x = suf; return n.size() >= x.size() && n.compare(n.size() - x.size(), x.size(), x) == 0; };
                if (ends("ROTATE")) { seqRotate = s.name; briefFrames[0] = std::max<int>(1, int(s.frames.size())); }
                else if (ends("PAN") && n != "PANMASK") { seqPan = s.name; briefFrames[1] = std::max<int>(1, int(s.frames.size())); }
                else if (ends("SOLAR")) { seqSolar = s.name; briefFrames[2] = std::max<int>(1, int(s.frames.size())); }
            }
        } catch (...) {}
        // camps/briefs/<brief>.txt raw: &X opens colour X, a lone & closes it, 0xFF ends.
        std::string text;
        try { const auto b = vfs_.read("camps/briefs/" + brief + ".txt"); text.assign(b.begin(), b.end()); } catch (...) {}
        if (const auto ff = text.find('\xff'); ff != std::string::npos) text.resize(ff);
        // The colour letters [U: retail's table not traced]: Y yellow, R red, G green, B blue.
        auto colourOf = [](char c) -> SDL_Color {
            switch (std::toupper(static_cast<unsigned char>(c))) {
            case 'Y': return {255, 255, 0, 255}; case 'R': return {255, 64, 64, 255};
            case 'G': return {64, 255, 64, 255}; case 'B': return {96, 160, 255, 255};
            default: return {255, 255, 255, 255};
            }
        };
        const SDL_Color plain{220, 220, 220, 255};
        const float maxW = 356;
        SDL_Color colour = plain;
        std::vector<BriefLine> line;
        float lineW = 0;
        auto flush = [&]() { briefLines.push_back(std::move(line)); line.clear(); lineW = 0; };
        std::string word;
        auto emitWord = [&]() {
            if (word.empty()) return;
            const float w = float(briefFont->width(word + " "));
            if (lineW + w > maxW && !line.empty()) flush();
            line.push_back({word + " ", colour});
            lineW += w;
            word.clear();
        };
        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '&') {
                emitWord();
                if (i + 1 < text.size() && std::isalpha(static_cast<unsigned char>(text[i + 1]))) colour = colourOf(text[++i]);
                else colour = plain;
            } else if (c == '\n') { emitWord(); flush(); }
            else if (c == '\r') {}
            else if (c == ' ' || c == '\t') emitWord();
            else word.push_back(c);
        }
        emitWord();
        if (!line.empty()) flush();
    };
    auto syncEnd = [&]() {
        const bool won = outcome_ > 0;
        const bool canContinue = !won || camp.cur + 1 < int(camp.missions.size());
        view->setBackdrop(std::string("bitmaps/") + (canContinue ? "outcome1" : "outcome0") + ".pcx");
        for (const char* g : {"Start", "Missions", "KNOB", "Difficulty", "AdjustDiff"}) view->setVisible(g, canContinue);
        view->setGrayed("LoadGame", true);   // save games are not implemented yet
        view->setGrayed("SaveGame", true);
        fillMissions(*view, camp, true);
        view->setSelection("Missions", std::clamp(camp.cur + (won ? 1 : 0), 0, std::max(0, int(camp.missions.size()) - 1)));
        view->setStage("Difficulty", camp.difficulty);
    };
    auto goPage = [&](Page p) {
        page_ = p;
        popup.reset();
        switch (p) {
        case Page::NewGame: open("newgame", "newcampaign4x"); syncNewGame(); break;
        case Page::Brief: open("msnbrief", nullptr); openBrief(); break;
        case Page::EndMission: open("endmsn", nullptr); syncEnd(); break;
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
#ifndef NDEBUG
    // TA_FRONT_ENDMSN=<+1|-1>: open ENDMSN after Arm Campaign mission 1 (front-end checks).
    static bool glamourOnce = false;   // TA_FRONT_GLAMOUR=<name>: the victory picture once
    if (const char* g = std::getenv("TA_FRONT_GLAMOUR"); g && !glamourOnce) { glamourOnce = true; showGlamour(g, g); }
    static bool endmsnOnce = false;
    if (const char* e = std::getenv("TA_FRONT_ENDMSN"); e && !endmsnOnce && start_ == Page::Main && loadTaCampaign(vfs_, "Arm Campaign", camp)) {
        endmsnOnce = true;
        camp.cur = 0;
        outcome_ = std::atoi(e) > 0 ? 1 : -1;
        camp.results[0] = outcome_ > 0 ? 'W' : 'L';
        start_ = Page::EndMission;
    }
#endif
    goPage(start_);
    start_ = Page::Main;
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
    std::string message = std::exchange(failure_, {});
    for (;;) {
        int w = 640, h = 480;
        SDL_GetRendererOutputSize(ren_, &w, &h);
        const GuiLayout L(w, h);
        SDL_Event e;
        auto onHit = [&](const std::string& h2) -> std::optional<Choice> {
            message.clear();   // any press dismisses the last message
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
            else if (h2 == "NEWCAMP") goPage(Page::NewGame);
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
        case Page::NewGame:
            if (h2 == "PREVMENU") goPage(Page::Single);
            else if (h2 == "ARM" || h2 == "SIDE0") { camp.side = 0; syncNewGame(); }
            else if (h2 == "CORE" || h2 == "SIDE1") { camp.side = std::min(1, std::max(0, sides::count() - 1)); syncNewGame(); }
            else if (h2 == "CAMPAIGN") {
                const int sel = view->selection("Campaign");
                if (sel >= 0 && sel < int(campNames.size())) loadTaCampaign(vfs_, campNames[size_t(sel)], browse);
            } else if (h2 == "START") {
                // 0x477b6d: results reset to U x 25, start at mission 0.
                if (!browse.active()) { message = "No campaign for this side."; break; }
                const int side = camp.side;
                camp = browse;
                camp.side = side;
                camp.results.assign(25, 'U');
                camp.cur = 0;
                camp.difficulty = view->stage("Difficulty");
                goPage(Page::Brief);
            }
            break;
        case Page::Brief:
            if (h2 == "SHUTUP") {
                if (view->stage("SHUTUP") == 0 && !narrationWav.empty()) narration.play(vfs_, narrationWav);
                else narration.stop();
            }
            if (h2 != "SHUTUP") narration.stop();
            if (h2 == "PREVMENU") goPage(Page::Single);   // retail: state 3
            else if (h2 == "START") return Choice::Campaign;
            break;
        case Page::EndMission:
            camp.difficulty = view->stage("Difficulty");   // applies at the next load
            if (h2 == "MAINMENU") return Choice::Menu;
            else if (h2 == "START") {
                const int sel = view->selection("Missions");
                if (sel >= 0 && sel < int(camp.missions.size())) { camp.cur = sel; goPage(Page::Brief); }
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
        // first draws (headless front-end runs); a SHOT:<file.png> step saves the page.
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
        static std::string shotPath;
        if (++frame > 3 && !script.empty()) {
            const std::string name = script.front();
            script.erase(script.begin());
            if (name.rfind("SHOT:", 0) == 0) shotPath = name.substr(5);   // saved after this frame draws
            else if (name == "WAIT") {}                                   // one frame (animations)
            else {
                TaGuiView& active = popup ? *popup : *view;
                const std::string got = active.activate(name);
                std::fprintf(stderr, "front-end script: %s -> %s\n", name.c_str(), got.empty() ? "(none)" : got.c_str());
                if (!got.empty()) { const auto r = onHit(upper(got)); if (r) return *r; }
            }
        }
#endif
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return Choice::Exit;
            if (page_ == Page::Brief && e.type == SDL_MOUSEWHEEL)
                briefScroll = std::clamp(briefScroll - e.wheel.y * 3, 0, std::max(0, int(briefLines.size()) - 8));
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
        if (page_ == Page::Brief && view && briefFont) {
            const int frame = int(SDL_GetTicks() / 66);
            if (!seqRotate.empty()) view->drawSequence(briefGaf, seqRotate, frame % briefFrames[0], 0, 0, L);
            if (!seqPan.empty()) view->drawSequence(briefGaf, seqPan, frame % briefFrames[1], 45, 130, L);
            if (!seqSolar.empty()) view->drawSequence(briefGaf, seqSolar, frame % briefFrames[2], 415, 0, L);
            // TextRegion 48,285 360x148; the wheel scrolls it.
            const int lh = std::max(10, briefFont->height() + 1);
            const int rows = 148 / lh;
            for (int r = 0; r < rows && briefScroll + r < int(briefLines.size()); ++r) {
                float x = 48;
                for (const auto& run : briefLines[size_t(briefScroll + r)]) {
                    briefFont->draw(ren_, L, x, float(285 + r * lh), run.text, run.color);
                    x += float(briefFont->width(run.text));
                }
            }
        }
        if (page_ != Page::Skirmish && !message.empty() && view) {
            // A failed launch's reason (or a NEWGAME refusal), on a dark band at the foot.
            const SDL_FRect band = L.rect(0, 462, 640, 18);
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 200);
            SDL_RenderFillRectF(ren_, &band);
            rowFont.draw(ren_, L, 8, 465, message, SDL_Color{255, 220, 120, 255});
        }
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
#ifndef NDEBUG
        if (!shotPath.empty()) {
            std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
            if (SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_ABGR8888, px.data(), w * 4) == 0)
                png::write(shotPath, w, h, px);
            std::fprintf(stderr, "front-end shot: %s\n", shotPath.c_str());
            shotPath.clear();
        }
#endif
        SDL_RenderPresent(ren_);
        SDL_Delay(10);
    }
}

}  // namespace ta
