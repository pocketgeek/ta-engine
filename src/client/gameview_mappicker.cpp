#include "client/gameview.h"
#include "client/mapfilter.h"
#include "net/roomseats.h"
#include <cmath>

// Out-of-line GameView method definitions (the game room's CHANGE MAP panel): a panel over
// the room with the installed maps' list (EXISTING MAP) and the random-map generator (RANDOM
// MAP), whose PICK asks the server to change the room's map (MpClient::setRoomMap).

namespace {
// The panel, in canvas units: over the room's right column, with the slot table's first
// 220 px left in view (dimmed) at its left.
constexpr SDL_FRect kPanel{260, 44, 680, 480};
const SDL_Color kDimText{150, 155, 165, 255}, kRed{255, 130, 110, 255}, kAmber{235, 205, 120, 255};

// Two ids name one map: a recipe exactly, a file map in any case (the data finds a map by
// its name whatever its case, and the server's map id is what the client sent).
bool sameMapId(const std::string& a, const std::string& b) {
    if (tak::mapgen::isGeneratedMapId(a) || tak::mapgen::isGeneratedMapId(b)) return a == b;
    return tak::mapfilter::lower(a) == tak::mapfilter::lower(b);
}
// The map list's row named `name` (any case), or -1.
template <class List> int listRow(const List& list, const std::string& name) {
    for (size_t i = 0; i < list.size(); ++i)
        if (sameMapId(list[i].name, name)) return int(i);
    return -1;
}
// The rows the FILTER leaves on show, as indices into the list.
template <class List> std::vector<int> shownRows(const List& list, const std::string& filter) {
    std::vector<int> shown;
    const std::string want = tak::mapfilter::lower(filter);
    for (int i = 0; i < int(list.size()); ++i)
        if (tak::mapfilter::matches(list[size_t(i)].title, list[size_t(i)].name, want)) shown.push_back(i);
    return shown;
}
// The first row on show for list row `row`: a few above it, so it is in view (0 when filtered out).
template <class List> int scrollFor(const List& list, const std::string& filter, int row) {
    const auto shown = shownRows(list, filter);
    const auto at = std::find(shown.begin(), shown.end(), row);
    return at == shown.end() ? 0 : std::max(0, int(at - shown.begin()) - 3);
}
// MAP SCENARIO belongs to one map: when the EXISTING candidate changes, its choice starts OFF
// and its facts are read again.
template <class Picker, class Facts> void syncScenario(Picker& p, Facts&& facts) {
    if (p.scenarioFor == p.file.path) return;
    p.scenarioFor = p.file.path;
    p.scenario = false;
    p.scenarioInfo = p.file.path.empty() || tak::mapgen::isGeneratedMapId(p.file.path) ? tak::sim::ScenarioInfo{}
                                                                                         : facts(p.file.path);
}
// "6, 7": seats as the slot table numbers them.
std::string seatNumbers(const std::vector<int>& slots) {
    std::string out;
    for (int s : slots) out += (out.empty() ? "" : ", ") + std::to_string(s + 1);
    return out;
}
int seatedHumans(const tak::net::RoomView& room) {
    int n = 0;
    for (const auto& s : room.slots) n += s.type == 1;
    return n;
}
}  // namespace

    // ---- the list, the generator and the preview ----

    void GameView::drawMapListBox(const SDL_FRect& area, MapPick& sel) {
        // A map with fewer seats than the room's seated humans is greyed with its player count
        // in red; it can still be selected, to preview it or to turn its MAP SCENARIO on (which
        // may open more seats).
        const int humans = seatedHumans(mpRoom());
        // Sort buttons: NAME / PLAYERS / SIZE. Clicking sets the sort key; clicking the
        // active one flips ascending/descending. The active key shows an up/down arrow.
        {
            const char* keys[3] = {"NAME", "PLAYERS", "SIZE"};
            const float bw = 86, bh = 20, sy = area.y;
            // FILTER: a text field right of the sort buttons, as tall as they are, to the
            // list's right edge. A dim FILTER until something is typed.
            {
                const float fx = area.x + 3 * (bw + 6), fw = area.w - 3 * (bw + 6);
                const SDL_FRect r{fx, sy, fw, bh};
                const bool active = lbField_ == 6;
                SDL_SetRenderDrawColor(ren_, 24, 26, 34, 255); SDL_RenderFillRectF(ren_, &r);
                SDL_SetRenderDrawColor(ren_, active ? 200 : 90, active ? 210 : 100, active ? 130 : 130, 255);
                SDL_RenderDrawRectF(ren_, &r);
                if (mapFilter_.empty() && !active)
                    blockText("FILTER", fx + 6, sy + 5, 1.5f, {110, 116, 132, 255});
                else
                    blockText(tak::uiTextFitStart(mapFilter_ + (active ? "_" : ""), 1.5f, fw - 12), fx + 6, sy + 5, 1.5f,
                              {225, 230, 240, 255});
                lobbyHots_.push_back({r, [this] { lbField_ = 6; SDL_StartTextInput(); }});
            }
            for (int k = 0; k < 3; ++k) {
                float bx = area.x + k * (bw + 6);
                SDL_FRect b{bx, sy, bw, bh};
                bool active = mapSort_ == k;
                SDL_SetRenderDrawColor(ren_, active ? 56 : 34, active ? 74 : 38, active ? 96 : 50, 255);
                SDL_RenderFillRectF(ren_, &b);
                SDL_SetRenderDrawColor(ren_, 90, 100, 130, 255); SDL_RenderDrawRectF(ren_, &b);
                const SDL_Color color = active ? SDL_Color{215, 230, 245, 255}
                                               : SDL_Color{170, 178, 195, 255};
                blockText(keys[k], bx + 6, sy + 5, 1.5f, color);
                if (active) {
                    const float ax = bx + bw - 10, ay = sy + bh / 2;
                    const float direction = mapSortDir_ > 0 ? -1.0f : 1.0f;
                    const SDL_Vertex arrow[] = {
                        {{ax, ay + direction * 3}, color, {}},
                        {{ax - 4, ay - direction * 3}, color, {}},
                        {{ax + 4, ay - direction * 3}, color, {}}
                    };
                    SDL_RenderGeometry(ren_, nullptr, arrow, 3, nullptr, 0);
                }
                lobbyHots_.push_back({b, [this, k](int) {
                    if (mapSort_ == k) mapSortDir_ = -mapSortDir_;   // toggle direction
                    else { mapSort_ = k; mapSortDir_ = 1; }
                    sortMapList();
                    mapScroll_ = 0;
                    rememberRoomSort();
                }});
            }
        }
        // The list under them, to the bottom of the area.
        const float boxX = area.x, boxY = area.y + 26, boxW = area.w, boxH = area.h - 26, rowH = 24, sbW = 12;
        // The rows on show: every map, or those whose title or name holds the FILTER text
        // (case-insensitive).
        const std::vector<int> shown = shownRows(mapList_, mapFilter_);
        const int total = int(shown.size());
        mapVisRows_ = int(boxH / rowH);
        mapTotalRows_ = total;
        clampMapScroll();
        mapListRect_ = {boxX, boxY, boxW, boxH};
        // box background + border
        SDL_FRect box{boxX, boxY, boxW, boxH};
        SDL_SetRenderDrawColor(ren_, 24, 26, 34, 255); SDL_RenderFillRectF(ren_, &box);
        SDL_SetRenderDrawColor(ren_, 70, 76, 96, 255); SDL_RenderDrawRectF(ren_, &box);
        const float rowW = boxW - sbW - 4;
        const float infoW = 108;                    // right-side "S 2P 8x8" column
        const float nameW = rowW - infoW - 12;      // the name column, at px 2.0
        for (int r = 0; r < mapVisRows_; ++r) {
            const int at = mapScroll_ + r;
            if (at >= total) break;
            const MapInfo& m = mapList_[size_t(shown[size_t(at)])];
            const std::string& nm = m.name;
            const std::string& pth = m.path;
            const bool picked = sameMapId(nm, sel.id);
            const bool grey = humans > seatsFor(m.starts, m.scenario, false);
            SDL_FRect row{boxX + 2, boxY + r * rowH, rowW, rowH};
            bool hot = lbHot(row);
            SDL_Color rc = picked ? SDL_Color{44, 78, 44, 255}
                         : hot ? SDL_Color{40, 46, 62, 255} : SDL_Color{24, 26, 34, 255};
            SDL_SetRenderDrawColor(ren_, rc.r, rc.g, rc.b, 255); SDL_RenderFillRectF(ren_, &row);
            const std::string& title = m.title.empty() ? nm : m.title;
            std::string label = tak::uiTextFit(title, 2.0f, nameW, false);
            // Keep part of a generated map's recipe suffix visible even when a
            // long title needs shortening, so duplicate titles can be told apart.
            if (label != title && title.size() > 17 &&
                title[title.size()-17] == '-' &&
                std::all_of(title.end()-16,title.end(),[](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                })) {
                const std::string tail = "~" + title.substr(title.size()-8);
                label = tak::uiTextFit(title, 2.0f, nameW - blockWidth(tail, 2.0f), false) + tail;
            }
            const SDL_Color greyText{130, 135, 150, 255};
            blockText(label,
                      boxX + 8, row.y + (rowH - 14) / 2, 2.0f,
                      grey ? greyText : picked ? SDL_Color{200, 240, 200, 255} : SDL_Color{220, 225, 235, 255});
            // Right-aligned player count + size, e.g. "4P 16x16".
            char players[12], size[16] = "";
            std::snprintf(players, sizeof players, "%dP", m.players);
            if (m.sizeW > 0) std::snprintf(size, sizeof size, " %dx%d", m.sizeW, m.sizeH);
            const SDL_Color infoColor = grey ? greyText : picked ? SDL_Color{170, 210, 170, 255} : SDL_Color{150, 160, 178, 255};
            const float infoX = boxX + rowW - blockWidth(std::string(players) + size, 1.5f) - 6;
            blockText(players, infoX, row.y + (rowH - 11) / 2, 1.5f, grey ? kRed : infoColor);
            blockText(size, infoX + blockWidth(players, 1.5f), row.y + (rowH - 11) / 2, 1.5f, infoColor);
            // MAP SCENARIO: a gold S marks a map with a playable scenario (or a Test Map copy).
            if (scenarioOffered(m.scenario) || (m.scenario.marker && m.scenario.runnable()))
                blockText("S", infoX - 14, row.y + (rowH - 11) / 2, 1.5f, {230, 200, 120, 255});
            lobbyHots_.push_back({row, [this, &sel, nm, pth] {
                sel = {nm, pth};
                picker_.message.clear();   // a new candidate: the last PICK's answer is old news
            }});
        }
        // Scrollbar: track + a proportional, draggable thumb (also wheel-scrollable).
        if (total > mapVisRows_) {
            float trackX = boxX + boxW - sbW;
            SDL_FRect track{trackX, boxY, sbW, boxH};
            SDL_SetRenderDrawColor(ren_, 30, 32, 42, 255); SDL_RenderFillRectF(ren_, &track);
            float thumbH = std::max(24.0f, boxH * mapVisRows_ / total);
            float thumbY = boxY + (boxH - thumbH) * mapScroll_ / float(total - mapVisRows_);
            mapThumbRect_ = {trackX, thumbY, sbW, thumbH};
            SDL_SetRenderDrawColor(ren_, mapDrag_ ? 130 : 90, mapDrag_ ? 150 : 100,
                                   mapDrag_ ? 190 : 130, 255);
            SDL_RenderFillRectF(ren_, &mapThumbRect_);
        } else {
            mapThumbRect_ = {0, 0, 0, 0};
        }
    }

    void GameView::drawGeneratorControls(float x, float y) {
        // One rhythm for the whole panel: every control is kRowH tall and every gap
        // is kGap. The rows used to step by 30, 30, 36 and then 44, so the buttons
        // sat tighter than the sliders and the run of sliders drifted out of line
        // with everything above it.
        const float kRowH = 24, kGap = 6, kBtnW = 300;
        const float kSliderH = 30;   // label (11) + 5 + bar (14)
        const float kHalfW = (kBtnW - kGap) / 2;
        float px = x, py = y;
        lbField(px, py, kBtnW, "MAP NAME (OPTIONAL)", genParams_.name, 5); py += 44;
        lbCycle(px, py, kBtnW, kRowH, std::string("TYPE:  ") + tak::mapgen::worldTitle(genParams_.mapType),
              true, [this](int direction) { genParams_.mapType = uint8_t((genParams_.mapType + direction + tak::mapgen::kMapTypes) % tak::mapgen::kMapTypes);
                             genParams_.layout=tak::mapgen::layoutForSlot(genParams_.mapType,tak::mapgen::slotOfLayout(genParams_.layout));
                             applyGenParams(); }); py += kRowH + kGap;
        // Mainland, Lakes, Islands (the volcano's Industrial Zone) and the world's own layout.
        lbCycle(px, py, kBtnW, kRowH, std::string("LAYOUT:  ") + tak::mapgen::layoutName(genParams_.layout),
              true, [this](int direction) { const int next=(tak::mapgen::slotOfLayout(genParams_.layout)+direction+4)%4;
                             genParams_.layout=tak::mapgen::layoutForSlot(genParams_.mapType,next);
                             applyGenParams(); }); py += kRowH + kGap;
        char pl[32]; std::snprintf(pl, sizeof pl, "PLAYERS:  %d", int(genParams_.players));
        lbCycle(px, py, kBtnW, kRowH, pl, true, [this](int direction) {
            genParams_.players = uint8_t(2 + (genParams_.players - 2 + direction + 7) % 7);
            applyGenParams();
        }); py += kRowH + kGap;
        // WIDTH and HEIGHT, in sections (32 cells), each on its own: a step offers only the
        // sizes the layout can build with the other side as it is (sanitize keeps both).
        const auto sizeCycle = [this](bool height, int direction) {
            const int cur = (height ? genParams_.heightCells : genParams_.widthCells) / 32;
            std::vector<int> sizes;
            for (int size : tak::mapgen::kSizeSteps) {
                auto candidate = genParams_;
                (height ? candidate.heightCells : candidate.widthCells) = uint16_t(size * 32);
                const auto kept = tak::mapgen::sanitize(candidate);
                if (kept.widthCells == candidate.widthCells && kept.heightCells == candidate.heightCells)
                    sizes.push_back(size);
            }
            if (sizes.empty()) return;
            const auto at = std::find(sizes.begin(), sizes.end(), cur);
            const int index = at == sizes.end() ? (direction > 0 ? -1 : 0) : int(at - sizes.begin());
            const int next = sizes[size_t((index + direction + int(sizes.size())) % int(sizes.size()))];
            (height ? genParams_.heightCells : genParams_.widthCells) = uint16_t(next * 32);
            applyGenParams();
        };
        char wl[32]; std::snprintf(wl, sizeof wl, "WIDTH:  %d", genParams_.widthCells / 32);
        lbCycle(px, py, kHalfW, kRowH, wl, true, [sizeCycle](int direction) { sizeCycle(false, direction); });
        char hl[32]; std::snprintf(hl, sizeof hl, "HEIGHT:  %d", genParams_.heightCells / 32);
        lbCycle(px + kHalfW + kGap, py, kHalfW, kRowH, hl, true, [sizeCycle](int direction) { sizeCycle(true, direction); });
        py += kRowH + kGap * 2;   // a wider break between the pickers and the sliders
        // Nine sliders in two half-width columns; each level reads at the right end of
        // its label line, where the bar's column ends.
        int column = 0;
        auto slider = [&](int idx, const char* label, uint8_t val, bool enabled = true, const char* off = "AUTO") {
            const float bx = px + column * (kHalfW + kGap), by = py + 16, bw = kHalfW, bh = 14;
            genSliderRect_[idx] = enabled ? SDL_FRect{bx, by, bw, bh} : SDL_FRect{};
            SDL_FRect bar{bx, by, bw, bh};
            SDL_SetRenderDrawColor(ren_, 30, 34, 46, 255); SDL_RenderFillRectF(ren_, &bar);
            float t = enabled ? val / 255.0f : 0.0f;
            SDL_FRect fill{bx, by, bw * t, bh};
            SDL_SetRenderDrawColor(ren_, 70, 120, 90, 255); SDL_RenderFillRectF(ren_, &fill);
            SDL_FRect handle{bx + bw * t - 3, by - 2, 6, bh + 4};
            SDL_SetRenderDrawColor(ren_, 200, 220, 200, 255); if (enabled) SDL_RenderFillRectF(ren_, &handle);
            SDL_SetRenderDrawColor(ren_, 70, 76, 96, 255); SDL_RenderDrawRectF(ren_, &bar);
            static const char* kScatterShort[tak::mapgen::kScatterLevels] = {"NONE", "MIN", "MED", "HIGH", "MAX"};
            const std::string level = !enabled ? off : idx==2 ? "+" + std::to_string(tak::mapgen::extraManaRounds(val)) + "/P" :
                idx==5 ? kScatterShort[std::min<int>(genParams_.manaScatter, tak::mapgen::kScatterLevels - 1)] :
                val == 0 ? "NONE" : val < 86 ? "LOW" : val < 171 ? "MED" : "HIGH";
            const float levelW = blockWidth(level, 1.3f);
            // The label shrinks, if it must, to leave the level its room on the line.
            blockText(label, bx, py + 2, tak::uiTextFitPx(label, 1.3f, bw - levelW - 8), {180, 185, 195, 255});
            blockText(level, bx + bw - levelW, py + 2, 1.3f, {160, 165, 180, 255});
            if (++column == 2) { column = 0; py += kSliderH + kGap; }
        };
        const uint8_t world = genParams_.mapType, layout = genParams_.layout;
        // The volcano has no trees or hill sections; its rocks are ruined Creon works.
        slider(0, "FORESTS", genParams_.treeDensity, tak::mapgen::worldHasForests(world), "NONE");
        slider(1, tak::mapgen::lavaWorld(world) ? "RUINS" : "ROCKS", genParams_.rockDensity);
        slider(2, "EXTRA MANA", genParams_.manaDensity);
        // Neutral spots scattered by AREA, apart from the per-player extra spots (a
        // level, not a count: the same level puts four times as many on a map twice as
        // wide). Five notches, NONE..MAX; MAX = a six-spot home's density.
        slider(5, "SCATTERED MANA", uint8_t(genParams_.manaScatter * 255 / (tak::mapgen::kScatterLevels - 1)));
        // Industrial Zone's lava is its own; the slider sets its cut-off pads instead.
        const bool pads = layout == tak::mapgen::IndustrialZone;
        slider(3, pads ? "LAVA PADS" : tak::mapgen::lavaWorld(world) ? "LAVA" : "WATER", genParams_.waterDensity,
               pads || !tak::mapgen::automaticWater(layout));
        slider(4, layout==tak::mapgen::Highlands?"HIGHLANDS":"HILLS", genParams_.reliefDensity,
               tak::mapgen::hillsPossible(world, layout), layout==tak::mapgen::Maze ? "AUTO" : "NONE");
        // v10 retail kits. Maze walls and Creon highlands are the layout's own high
        // ground; Veruna ships no mountain kit.
        slider(6, "PLATEAUS", genParams_.plateauDensity, !tak::mapgen::automaticPlateaus(layout) &&
               tak::mapgen::kitAvailable(world, tak::mapgen::KitPlateaus));
        slider(7, "MOUNTAINS", genParams_.mountainDensity, tak::mapgen::kitAvailable(world, tak::mapgen::KitMountains), "NONE");
        slider(8, world==tak::mapgen::Zhon?"VILLAGES":"TOWNS", genParams_.townDensity,
               tak::mapgen::kitAvailable(world, tak::mapgen::KitTowns), "NONE");
        // The seed button shares the last row with TOWNS.
        py += 8;
        px += kHalfW + kGap;
        lbBtn(px, py, kHalfW, kRowH, "RE-ROLL SEED", true, [this] {
            genParams_.seed = genParams_.seed * 6364136223846793005ULL + 1442695040888963407ULL;
            applyGenParams();
        });
    }

    void GameView::drawPickPreview(const SDL_FRect& box, const std::string& id, const std::string& path, bool withScenario) {
        // A real map shows its embedded minimap; a generated map shows a live thumbnail
        // rolled from its seed (both go through buildMapPreview -> mapPreviewTex_). The
        // dims line goes under it.
        const bool gen = tak::mapgen::isGeneratedMapId(id);
        const float pvx = box.x, pvy = box.y, pvW = box.w, pvH = box.h;
        const SDL_FRect pbox = box;
        SDL_SetRenderDrawColor(ren_, 18, 20, 28, 255); SDL_RenderFillRectF(ren_, &pbox);
        if (mapPreviewFor_ != path) buildMapPreview(path);
        if (mapPreviewTex_ && mapPreviewW_ > 0) {
            float sc = std::min(pvW / float(mapPreviewW_), pvH / float(mapPreviewH_));
            float iw = mapPreviewW_ * sc, ih = mapPreviewH_ * sc;
            SDL_FRect dst{pvx + (pvW - iw) / 2, pvy + (pvH - ih) / 2, iw, ih};
            SDL_RenderCopyF(ren_, mapPreviewTex_, nullptr, &dst);
            drawStartMarkers(dst);   // no slots yet: the room tints them
        } else if (gen && !genPreviewError_.empty()) {
            // A recipe the retail data cannot build: the reason in the box.
            for (const auto& [line, y, color] : {std::tuple{"MAP UNAVAILABLE", -16.0f, SDL_Color{180, 185, 195, 255}},
                                                 std::tuple{"CHECK RETAIL DATA", 4.0f, SDL_Color{220, 140, 120, 255}}})
                blockText(line, pvx + (pvW - blockWidth(line, 1.4f)) / 2, pvy + pvH / 2 + y, 1.4f, color);
        } else {
            const char* none = gen ? "GENERATING" : "NO PREVIEW";
            blockText(none, pvx + (pvW - blockWidth(none, 1.6f)) / 2, pvy + pvH / 2 - 7, 1.6f,
                      {120, 125, 140, 255});
        }
        SDL_SetRenderDrawColor(ren_, 70, 76, 96, 255); SDL_RenderDrawRectF(ren_, &pbox);
        if (gen && genPreviewPending_ && mapPreviewTex_) {   // the last map's picture, until the new one is rolled
            const char* note = "GENERATING";
            const SDL_FRect band{pvx, pvy + pvH / 2 - 14, pvW, 28};
            SDL_SetRenderDrawColor(ren_, 12, 14, 20, 200); SDL_RenderFillRectF(ren_, &band);
            blockText(note, pvx + (pvW - blockWidth(note, 1.6f)) / 2, pvy + pvH / 2 - 7, 1.6f, {200, 205, 220, 255});
        }
        if (mapPreviewTex_ && mapPreviewW_ > 0 && !genPreviewPending_) {
            lobbyHots_.push_back({pbox, [this] { openBigMap(); }});   // full window
            drawClickHint(pbox);
        }
        if (!mapPreviewDims_.empty())
            blockText(mapPreviewDims_, pvx, pvy + pvH + 8, 1.6f, {160, 165, 180, 255});
        // MAP SCENARIO: play the map's own scenario -- its placed armies,
        // trigger rules and Use Only list -- instead of a skirmish. Off for every map until
        // chosen; greyed, with the reason, when the map has no playable scenario. The lines
        // under it say what turning it on changes (dim while it is off). The choice is the
        // EXISTING candidate's.
        if (!withScenario || gen || path.empty()) return;
        bool& chosen = picker_.scenario;
        const tak::sim::ScenarioInfo& info = picker_.scenarioInfo;
        const float y0 = pvy + pvH + 26, cycleH = 24;
        const bool testCopy = info.marker && info.runnable();
        // A Test Map copy of a map whose only scenario content is a Use Only list
        // (hasscenario stays 0) plays no scenario, but its opt-in still applies the list.
        const bool testUseOnly = info.marker && !info.hasScenario && info.useOnlyTypes >= 0;
        const bool offered = scenarioOffered(info);
        lbCycle(pvx, y0, pvW, cycleH, (chosen && offered) || testCopy ? "MAP SCENARIO: ON" : "MAP SCENARIO: OFF",
                offered, [this, flip = &chosen](int) { *flip = !*flip; picker_.message.clear(); });
        std::vector<std::pair<std::string, SDL_Color>> lines;
        const SDL_Color grey{150, 155, 165, 255}, dim{130, 135, 150, 255}, normal{205, 210, 225, 255};
        if (testCopy) {
            lines.push_back({"TEST COPY: ALWAYS ON", grey});
            for (const auto& l : tak::sim::scenarioSummary(info, true))
                lines.push_back({l.text, l.warning ? kAmber : normal});
        } else if (testUseOnly) {
            lines.push_back({"TEST COPY: USE ONLY LIST", grey});
            lines.push_back({tak::sim::scenarioUseOnlyLine(info, true), normal});
        } else if (!offered) {
            for (const auto& l : tak::sim::scenarioReason(info)) lines.push_back({l, grey});
        } else {
            // Offered only because a game mod pack may supply a missing unit type.
            if (!info.offerable()) {
                // The pack's own name (its capitals under MODERN), cut to the column.
                lines.push_back({"PACK NEEDED: " + info.detail, kAmber});
            }
            for (const auto& l : tak::sim::scenarioSummary(info, true))
                lines.push_back({l.text, !chosen ? dim : l.warning ? kAmber : normal});
        }
        // Four lines at a 16 pitch, or a fifth (a test copy's or the pack warning on
        // top of a full summary) at 14, each cut to the preview's width.
        const float pitch = lines.size() > 4 ? 14.0f : 16.0f;
        for (size_t i = 0; i < lines.size() && i < 5; ++i)
            blockText(tak::uiTextFit(lines[i].first, 1.2f, pvW - 2), pvx, y0 + cycleH + 6 + float(i) * pitch, 1.2f,
                      lines[i].second);
    }

    // ---- the CHANGE MAP panel ----

    void GameView::openMapPicker() {
        const auto& room = mpRoom();
        const tak::Settings::RoomPrefs pref = settings_ ? settings_->prefsFor(singlePlayer_) : tak::Settings::RoomPrefs{};
        const bool roomGen = tak::mapgen::isGeneratedMapId(room.mapId);
        if (!picker_.open) {
            if (!mp_->roomEditable()) { roomActError_ = "only the host changes the map, before the game starts"; return; }
            if (!roomForcedMap_.empty()) { roomActError_ = "a test map's room keeps its map"; return; }
            if (mp_->roomMapPending()) { roomActError_ = "the last map change is still waiting for the server"; return; }
            lbField_ = 0; SDL_StopTextInput();
            genSlider_ = -1; mapDrag_ = false;
            const uint32_t waiting = picker_.pickSerial;   // a PICK answered after a CANCEL still reports
            picker_ = MapPicker{};
            picker_.open = true;
            picker_.pickSerial = waiting;
            if (mapSort_ != pref.mapSort || mapSortDir_ != pref.mapSortDir) {
                mapSort_ = pref.mapSort; mapSortDir_ = pref.mapSortDir;
                sortMapList();
            }
            // RANDOM MAP: the room's recipe when it plays one, else the remembered recipe (one
            // this build can make), else the generator's defaults.
            genParams_ = {};
            if (roomGen) {
                picker_.random = true;
                try { genParams_ = tak::mapgen::parseMapId(room.mapId); } catch (const std::exception&) {}
                picker_.gen = {room.mapId, room.mapId};
            } else {
                if (tak::mapgen::isGeneratedMapId(pref.generator))
                    try { genParams_ = tak::mapgen::parseMapId(pref.generator); } catch (const std::exception&) { genParams_ = {}; }
                applyGenParams();   // the panel is open: its RANDOM candidate
            }
        }
        // EXISTING MAP: the room's file map when the list has it, else (a room on a random
        // map) the remembered map when listed; scrolled into view. Until the list is read
        // (READING MAPS...), drawMapPicker calls this again once it is.
        if (!mapListScanned_) return;
        if (picker_.file.id.empty()) {
            const int at = listRow(mapList_, roomGen ? pref.map : room.mapId);
            if (at < 0) return;
            const MapInfo& m = mapList_[size_t(at)];
            picker_.file = {m.name, m.path};
            if (!roomGen) {   // the room's own map, with its MAP SCENARIO as the room plays it
                picker_.scenarioFor = m.path;
                picker_.scenario = room.opts.mapScenario != 0;
                picker_.scenarioInfo = mapScenarioInfo(m.path);
            }
        }
        if (const int at = listRow(mapList_, picker_.file.id); at >= 0) mapScroll_ = scrollFor(mapList_, mapFilter_, at);
    }

    void GameView::closeMapPicker() {
        if (!picker_.open) return;
        // An unanswered PICK keeps its serial: a refusal then shows on the room's note line.
        picker_.open = false;
        pickerScanPending_ = false;
        if (lbField_ == 5 || lbField_ == 6) { lbField_ = 0; SDL_StopTextInput(); }
        genSlider_ = -1; mapDrag_ = false;
        mapListRect_ = mapThumbRect_ = SDL_FRect{};
        for (auto& rect : genSliderRect_) rect = {};
    }

    void GameView::pickerSelect(const std::string& id) {
        if (!picker_.open) { roomActError_ = "the map panel is not open"; return; }
        picker_.message.clear();
        if (tak::mapgen::isGeneratedMapId(id)) {
            // The recipe as given (a TAK_ROOM_SCRIPT step names the exact map it wants), shown
            // in the generator's controls.
            try { genParams_ = tak::mapgen::parseMapId(id); }
            catch (const std::exception& e) { roomActError_ = e.what(); return; }
            picker_.random = true;
            picker_.gen = {id, id};
            return;
        }
        // A file map: its list row (the name in any case), else wherever the data has it (the
        // list is read when the panel first draws).
        const int at = listRow(mapList_, id);
        const std::string path = at >= 0 ? mapList_[size_t(at)].path : tak::hpi::findMap(vfs_, id);
        if (path.empty() || tak::mapgen::isGeneratedMapId(path)) { roomActError_ = "map not installed: " + id; return; }
        picker_.random = false;
        picker_.file = {at >= 0 ? mapList_[size_t(at)].name : id, path};
        syncScenario(picker_, [this](const std::string& p) { return mapScenarioInfo(p); });
        if (at >= 0) mapScroll_ = scrollFor(mapList_, mapFilter_, at);
    }

    void GameView::pickerPick() {
        if (!picker_.open) { roomActError_ = "the map panel is not open"; return; }
        if (picker_.pickSerial) { roomActError_ = "the last map change is still waiting for the server"; return; }
        const auto& room = mpRoom();
        syncScenario(picker_, [this](const std::string& p) { return mapScenarioInfo(p); });
        const MapPick c = picker_.random ? picker_.gen : picker_.file;
        if (c.id.empty()) { roomActError_ = "no map is selected"; return; }
        if (picker_.random && mapPreviewFor_ == c.path && !genPreviewError_.empty()) {
            roomActError_ = "map unavailable: " + genPreviewError_; return;
        }
        // MAP SCENARIO: an offerable map's own scenario, when chosen. A Test Map copy plays
        // its scenario through its opt-in and sends 0 (the server checks the verified
        // package either way).
        const bool sc = !picker_.random && picker_.scenario && scenarioOffered(picker_.scenarioInfo);
        const bool same = sameMapId(c.id, room.mapId);
        if (same && uint8_t(sc) == room.opts.mapScenario && room.mapsReady) { closeMapPicker(); return; }
        // The seats it gives: the room's own map counts them from its verified package (the
        // file here may be another revision), any other map from this install.
        const RoomStarts starts = same && mp_->mapPackage() && !picker_.random ? roomStarts() : RoomStarts{};
        const uint8_t cap = starts.count >= 0 ? seatsFor(starts.count, scenarioLobby_, sc) : capacityFor(c.id, c.path, sc);
        const int humans = seatedHumans(room);
        if (humans > cap) {
            roomActError_ = "this map has " + std::to_string(cap) + " starts and " + std::to_string(humans) +
                            " players are seated";
            return;
        }
        // The host's chosen start mode, which the room takes back when MAP SCENARIO ends up off.
        const uint8_t startMode = roomChosenSet_ ? roomChosen_.randomStarts : room.opts.randomStarts;
        // The room's own map keeps the room's spelling of its name: the server sees no new map.
        if (!mp_->setRoomMap(same ? room.mapId : c.id, cap, sc, startMode)) {
            picker_.message = "CANNOT USE THIS MAP: " + mp_->roomError();
            roomActError_ = mp_->roomError();
            return;
        }
        picker_.message.clear();
        picker_.pickSerial = mp_->roomMapPending();
        if (!picker_.pickSerial) closeMapPicker();   // only offered the room's own map again
    }

    void GameView::pickerStep() {
        // The room moved on: this client is no longer its host, a START began, it left.
        if (picker_.open && !mp_->roomEditable()) closeMapPicker();
        if (picker_.open) syncScenario(picker_, [this](const std::string& p) { return mapScenarioInfo(p); });
        if (!picker_.pickSerial) return;
        const auto& answer = mp_->roomMapAnswer();
        if (answer.serial != picker_.pickSerial) {
            // Dropped unanswered (the room was left, the host changed, the game is starting).
            if (!mp_->roomMapPending()) picker_.pickSerial = 0;
            return;
        }
        picker_.pickSerial = 0;
        using A = tak::net::MpClient::RoomMapAnswer;
        if (answer.kind == A::Accepted) {
            const auto& room = mpRoom();
            rememberRoomPrefs(RoomField::Map);
            // A random map's preview is the room's now: the room takes it over rather than
            // rolling the map again.
            if (tak::mapgen::isGeneratedMapId(room.mapId) && mapPreviewFor_ == room.mapId)
                mapPreviewFor_ = "room:" + room.mapId + "#";
            closeMapPicker();
            return;
        }
        if (answer.kind == A::Refused && !picker_.open) {   // CANCEL came first: the room's note line says it
            roomRefusal_ = "CANNOT CHANGE THE MAP: " + answer.why;
            roomRefusalUntil_ = SDL_GetTicks64() + 6000;
        } else if (answer.kind == A::Refused) picker_.message = "CANNOT CHANGE THE MAP: " + answer.why;
        else if (answer.kind == A::TimedOut && picker_.open) picker_.message = "THE SERVER DID NOT ANSWER: TRY AGAIN";
    }

    bool GameView::pickerInput(const SDL_Event& e) {
        // Esc closes the panel, as CANCEL (with a field active it only ends the typing).
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !lbField_) {
            closeMapPicker();
            return true;
        }
        // A double click on a list row picks it: its first click selected it, the second
        // selects it again and PICKs.
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && e.button.clicks >= 2 &&
            !picker_.random) {
            mouseX_ = float(e.button.x); mouseY_ = float(e.button.y);
            float mx, my; lobbyMouse(mx, my);
            if (!ptIn(mapListRect_, mx, my) || mx > mapListRect_.x + mapListRect_.w - 16) return false;
            for (auto& hot : lobbyHots_)
                if (ptIn(hot.rect, mx, my) && hot.action) {
                    hot.action();
                    pickerPick();
                    return true;
                }
        }
        return false;
    }

    void GameView::drawMapPicker(int winW, int winH) {
        const auto& room = mpRoom();
        // Only the panel takes the pointer: what the room registered as it drew -- its
        // buttons, its chat box -- is dropped.
        lobbyHots_.clear();
        mapListRect_ = mapThumbRect_ = chatRect_ = SDL_FRect{};
        for (auto& rect : genSliderRect_) rect = {};
        const SDL_FRect dim{0, 0, float(winW), float(winH)};
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 160); SDL_RenderFillRectF(ren_, &dim);
        const SDL_FRect P = kPanel;
        SDL_SetRenderDrawColor(ren_, 28, 30, 40, 255); SDL_RenderFillRectF(ren_, &P);
        SDL_SetRenderDrawColor(ren_, 70, 76, 96, 255); SDL_RenderDrawRectF(ren_, &P);
        // The map list is read once per room (leaveRoom forgets it, so maps received in other
        // rooms appear): one frame says so, the next reads it.
        if (!mapListScanned_) {
            if (!pickerScanPending_) {
                pickerScanPending_ = true;
                const char* note = "READING MAPS...";
                blockText(note, P.x + (P.w - blockWidth(note, 2.0f)) / 2, P.y + P.h / 2 - 7, 2.0f, {200, 205, 220, 255});
                return;
            }
            buildMapList();
            pickerScanPending_ = false;
            openMapPicker();   // EXISTING MAP's candidate, now the list is known
        }
        // Tabs: each keeps its own candidate, so switching loses nothing.
        const auto tab = [&](float dx, const char* label, bool random) {
            const bool active = picker_.random == random;
            lbBtn(P.x + dx, P.y + 10, 150, 24, label, true, [this, random] {
                if (picker_.random == random) return;
                picker_.random = random;
                picker_.message.clear();
                if (random && picker_.gen.id.empty()) applyGenParams();
            }, active ? SDL_Color{56, 74, 96, 255} : SDL_Color{34, 38, 50, 255}, 1.6f);
        };
        tab(12, "EXISTING MAP", false);
        tab(168, "RANDOM MAP", true);
        if (!picker_.random) {
            drawMapListBox({P.x + 12, P.y + 44, 420, 366}, picker_.file);
            drawPickPreview({P.x + 448, P.y + 44, 220, 220}, picker_.file.id, picker_.file.path, true);
        } else {
            drawGeneratorControls(P.x + 12, P.y + 44);
            drawPickPreview({P.x + 332, P.y + 44, 336, 336}, picker_.gen.id, picker_.gen.path, false);
        }
        // The footer: CANCEL, PICK, and between them what a PICK of the candidate would do.
        const MapPick& c = picker_.random ? picker_.gen : picker_.file;
        const bool sc = !picker_.random && picker_.scenario && scenarioOffered(picker_.scenarioInfo);
        const int humans = seatedHumans(room);
        int cap = 0;
        if (tak::mapgen::isGeneratedMapId(c.id)) cap = std::clamp<int>(tak::mapgen::decodeMapId(c.id).players, 2, tak::net::kMaxSlots);
        else if (!c.id.empty()) {
            const int at = listRow(mapList_, c.id);
            cap = at >= 0 ? seatsFor(mapList_[size_t(at)].starts, picker_.scenarioInfo, sc) : capacityFor(c.id, c.path, sc);
        }
        const bool genError = picker_.random && mapPreviewFor_ == c.path && !genPreviewError_.empty();
        const bool unchanged = sameMapId(c.id, room.mapId) && uint8_t(sc) == room.opts.mapScenario;
        std::string text;
        SDL_Color color = kDimText;
        if (picker_.pickSerial) text = "CHANGING THE MAP...";
        else if (!picker_.message.empty()) { text = picker_.message; color = kRed; }
        else if (c.id.empty()) text = "SELECT A MAP";
        else if (humans > cap) {
            text = "THIS MAP HAS " + std::to_string(cap) + " STARTS: " + std::to_string(humans) + " PLAYERS ARE SEATED";
            color = kRed;
        } else if (genError) { text = "MAP UNAVAILABLE: CHECK RETAIL DATA"; color = kRed; }
        else if (!unchanged) {
            // What the server's capacity change would do to the seats (the plan it applies).
            const auto plan = tak::net::planCapacity(room.slots, cap);
            const size_t removed = plan.removedAi.size(), moved = plan.moves.size();
            const std::string players = std::to_string(moved) + (moved == 1 ? " PLAYER" : " PLAYERS");
            if (removed) {
                text = "PICK REMOVES " + std::to_string(removed) + (removed == 1 ? " AI PLAYER (SEAT " : " AI PLAYERS (SEATS ") +
                       seatNumbers(plan.removedAi) + ")" + (moved ? " AND MOVES " + players : "");
            } else if (moved) {
                text = "PICK MOVES " + players;
            }
            color = kAmber;
        }
        const bool pickOk = !picker_.pickSerial && !c.id.empty() && humans <= cap && !genError && mp_->roomEditable();
        const float fy = P.y + 440;
        lbBtn(P.x + 12, fy, 120, 28, "CANCEL", true, [this] { closeMapPicker(); });
        lbBtn(P.x + 548, fy, 120, 28, "PICK", pickOk, [this] { pickerPick(); }, {70, 110, 70, 255});
        if (!text.empty()) {
            constexpr float kTextW = 392;
            const float px = tak::uiTextFitPx(text, 1.3f, kTextW, 1.0f);
            const std::string shown = tak::uiTextFit(text, px, kTextW);
            blockText(shown, P.x + 144 + (kTextW - blockWidth(shown, px)) / 2, fy + 14 - 3.5f * px, px, color);
        }
    }
