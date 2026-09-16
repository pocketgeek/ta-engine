#include "client/gameview.h"

// Out-of-line GameView method definitions (lobby concern), split from the
// class body in gameview.h so editing a body recompiles only this translation
// unit. Trivial getters, ctors, static, template, constexpr and default-arg
// methods stay inline in the header. Grouping is by name heuristic.

    bool GameView::inLobbyPhase() const {
        if (!mp_ || mpSetupDone_) return false;
        auto s = mp_->state();
        return s == ta::net::MpClient::State::Connecting ||
               s == ta::net::MpClient::State::Lobby ||
               s == ta::net::MpClient::State::InRoom ||
               s == ta::net::MpClient::State::Done;
    }

    int GameView::geomSlot(int id) const {
        return (id >= 0 && size_t(id) < geomIndex_.size()) ? geomIndex_[size_t(id)] : -1;
    }

namespace {
// What to SHOW for a map id. A generated map's id is a "~gen1~<hex>" recipe -- the
// whole parameter block in hex -- which is unreadable in a lobby row. Decode it to
// the label mapgen already provides ("Random 16x16 4P Aramon"); a real map's id is
// its name and passes through.
std::string mapDisplayName(const std::string& id) {
    if (!ta::mapgen::isGeneratedMapId(id)) return id;
    return ta::mapgen::friendlyLabel(ta::mapgen::decodeMapId(id));
}
}  // namespace

    void GameView::drawLobby(int winW, int winH) {
        lobbyHots_.clear();
        // Map-picker geometry is only live while the create screen is shown; clear it
        // so a stale thumb/list rect can't grab clicks or wheel on the other screens.
        mapListRect_ = mapThumbRect_ = SDL_FRect{0, 0, 0, 0};
        // absorb any new chat
        if (mp_) for (auto& m : mp_->takeChat()) chatLog_.push_back(m);
        // The ground + centred panel frame are painted by the caller (the viewport is
        // already offset to this panel); RenderClear would ignore the viewport.
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        float cx = winW / 2.0f;
        const char* title = singlePlayer_ ? "SINGLE PLAYER VS AI" : "TOTAL ANNIHILATION  MULTIPLAYER";
        blockText(title, cx - blockWidth(title, 2.6f) / 2, 24, 2.6f, {210, 200, 150, 255});
        if (!mp_) return;
        auto st = mp_->state();
        if (st == ta::net::MpClient::State::Connecting) {
            blockText("connecting to server...", cx - 120, winH / 2.0f, 2.0f, {200, 200, 210, 255});
            return;
        }
        if (st == ta::net::MpClient::State::Done) {
            // Connection over (server closed / rejected us / timed out): say WHY --
            // MpClient preserves the server's Reject reason (e.g. "protocol version
            // mismatch (server 28 / client 29)") -- and give the player a way OUT.
            float my = winH / 2.0f - 40;
            blockText("DISCONNECTED", cx - blockWidth("DISCONNECTED", 2.6f) / 2, my,
                      2.6f, {255, 130, 110, 255});
            std::string why = mp_->error().empty() ? std::string("the server closed the connection")
                                                   : mp_->error();
            // Long reasons (version-mismatch text) wrap onto two centred lines.
            std::string l1 = why, l2;
            if (why.size() > 56) {
                size_t cut = why.rfind(' ', why.size() / 2 + 12);
                if (cut == std::string::npos) cut = why.size() / 2;
                l1 = why.substr(0, cut);
                l2 = why.substr(cut + 1);
            }
            blockText(l1, cx - blockWidth(l1, 1.8f) / 2, my + 34, 1.8f, {215, 218, 228, 255});
            if (!l2.empty())
                blockText(l2, cx - blockWidth(l2, 1.8f) / 2, my + 56, 1.8f, {215, 218, 228, 255});
            lbBtn(cx - 90, my + (l2.empty() ? 78.0f : 96.0f), 180, 32, "BACK TO MENU", true,
                  [this] { menuRequested_ = true; });
            return;
        }
        if (st == ta::net::MpClient::State::InRoom) drawRoom(winW, winH);
        else if (lobbyScreen_ == LobbyScreen::Create) drawCreate(winW, winH);
        else drawBrowser(winW, winH);
    }

    void GameView::drawBrowser(int winW, int winH) {
        (void)winH;
        // keep the list fresh without needing a manual refresh
        if (SDL_GetTicks64() - mpListMs_ > 1000) { mp_->listGames(); mpListMs_ = SDL_GetTicks64(); }
        float x = 60, y = 80, w = winW - 120.0f;
        blockText("GAMES", x, y, 2.2f, {200, 205, 220, 255});
        lbBtn(x + w - 200, y - 6, 95, 26, "REFRESH", true, [this] { mp_->listGames(); });
        lbBtn(x + w - 100, y - 6, 100, 26, "CREATE", true,
              [this] { lobbyScreen_ = LobbyScreen::Create; });
        y += 34;
        // column header
        blockText("NAME", x + 8, y, 1.6f, {130, 135, 150, 255});
        blockText("MAP", x + 220, y, 1.6f, {130, 135, 150, 255});
        blockText("PLAYERS", x + w - 200, y, 1.6f, {130, 135, 150, 255});
        y += 20;
        const auto& games = mp_->games();
        if (games.empty())
            blockText("no games -- create one, or refresh", x + 8, y + 10, 1.8f, {150, 150, 160, 255});
        for (const auto& g : games) {
            SDL_FRect row{x, y, w, 30};
            bool hot = lbHot(row) && !g.running;
            SDL_SetRenderDrawColor(ren_, hot ? 46 : 30, hot ? 52 : 34, hot ? 72 : 44, 255);
            SDL_RenderFillRectF(ren_, &row);
            blockText(g.name, x + 8, y + 8, 1.8f, {225, 228, 236, 255});
            blockText(mapDisplayName(g.mapId), x + 220, y + 8, 1.6f, {180, 185, 195, 255});
            char pc[32]; std::snprintf(pc, sizeof pc, "%d/%d", g.players, g.capacity);
            blockText(pc, x + w - 200, y + 8, 1.8f, {200, 205, 215, 255});
            if (g.passworded)
                blockText("LOCK", x + w - (g.running ? 232 : 120), y + 8, 1.6f, {230, 200, 120, 255});
            if (g.running) {
                // A running game can't be joined, but it can be watched live.
                blockText("LIVE", x + w - 158, y + 8, 1.6f, {230, 140, 120, 255});
                lbBtn(x + w - 96, y + 3, 92, 24, "WATCH", true,
                      [this, id = g.id] { mp_->spectate(id, joinPass_); });
            } else {
                lobbyHots_.push_back({row, [this, id = g.id] { mp_->joinGame(id, joinPass_); }});
            }
            y += 34;
        }
        // password entry for locked games
        lbField(x, winH - 70.0f, 200, "PASSWORD (for locked games)", joinPass_, 3);
        lbBtn(winW - 140.0f, winH - 40.0f, 120, 30, "BACK", true, [this] { menuRequested_ = true; });
    }

    void GameView::buildMapList() {
        mapList_.clear();
        for (auto& [name, path] : ta::hpi::listMaps(vfs_)) {
            MapInfo mi; mi.name = name; mi.path = path;
            std::filesystem::path op = path; op.replace_extension(".ota");
            std::string otaPath = op.generic_string();
            if (vfs_.has(otaPath)) {
                try {
                    auto b = vfs_.read(otaPath);
                    auto root = ta::tdf::parseText(std::string(b.begin(), b.end()), otaPath);
                    if (const auto* gh = root.child("globalheader")) {
                        mi.players = int(gh->numberOr("numplayers", 0));
                        std::string sz = gh->valueOr("size", "");   // e.g. "16 x 16"
                        int a = 0, c = 0;
                        if (std::sscanf(sz.c_str(), "%d x %d", &a, &c) == 2) { mi.sizeW = a; mi.sizeH = c; }
                    }
                } catch (const std::exception&) {}
            }
            if (mi.players == 0)   // .ota had no numplayers: count the start positions
                mi.players = int(ta::sim::parseStartPositions(vfs_, path).size());
            mapList_.push_back(std::move(mi));
        }
        sortMapList();
    }

    void GameView::sortMapList() {
        int dir = mapSortDir_, key = mapSort_;
        auto lname = [](const std::string& s) {
            std::string t = s; std::transform(t.begin(), t.end(), t.begin(), ::tolower); return t;
        };
        std::stable_sort(mapList_.begin(), mapList_.end(),
                         [&](const MapInfo& a, const MapInfo& b) {
            long long c = 0;
            if (key == 1) c = a.players - b.players;
            else if (key == 2) c = a.area() - b.area();
            if (c == 0) {   // tiebreak (and the whole order for NAME) by name A->Z
                std::string na = lname(a.name), nb = lname(b.name);
                if (na != nb) return dir > 0 ? na < nb : na > nb;
                return false;
            }
            return dir > 0 ? c < 0 : c > 0;
        });
    }

    void GameView::buildMapPreview(const std::string& tntPath) {
        if (mapPreviewTex_) { gpuvram::destroy(mapPreviewTex_); mapPreviewTex_ = nullptr; }
        mapPreviewFor_ = tntPath;
        mapPreviewW_ = mapPreviewH_ = 0;
        mapPreviewDims_.clear();
        if (tntPath.empty()) return;
        if (ta::mapgen::isGeneratedMapId(tntPath)) { buildGenPreview(tntPath); return; }
        std::vector<uint8_t> d;
        try { d = vfs_.read(tntPath); } catch (...) { return; }
        if (d.size() < 52) return;
        int w = 0, h = 0; const uint8_t* idx = nullptr;
        if (!tntIndexedImage(d, 12, w, h, idx) && !tntIndexedImage(d, 11, w, h, idx)) return;
        std::string ota = readOta(tntPath);
        const std::vector<uint8_t>* pal = kingdomPalette(otaField(ota, "kingdom"));
        if (!pal) pal = kingdomPalette("aramon");   // maps without a kingdom get a default
        if (!pal) return;
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            uint8_t p = idx[i];
            if (p == 9) { rgba[i * 4 + 3] = 0; continue; }   // retail's transparent index
            const uint8_t* c = &(*pal)[size_t(p) * 4];
            rgba[i * 4 + 0] = c[0]; rgba[i * 4 + 1] = c[1]; rgba[i * 4 + 2] = c[2]; rgba[i * 4 + 3] = 255;
        }
        mapPreviewTex_ = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!mapPreviewTex_) return;
        SDL_SetTextureBlendMode(mapPreviewTex_, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(mapPreviewTex_, nullptr, rgba.data(), w * 4);
        SDL_SetTextureScaleMode(mapPreviewTex_, SDL_ScaleModeLinear);
        mapPreviewW_ = w; mapPreviewH_ = h;
        // Info line under the preview, retail-style: "<size>  <N> PLAYER" (memory
        // dropped). The .ota supplies size ("9 x 7", already in 32-cell units) and
        // numplayers; derive them from the tnt / start positions if the .ota omits them.
        auto u32 = [&](size_t o) { return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8)
                 | (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24); };
        std::string size = otaField(ota, "size");
        if (size.empty()) size = std::to_string(u32(4) / 32) + " x " + std::to_string(u32(8) / 32);
        std::string players = otaField(ota, "numplayers");
        if (players.empty()) {
            int n = int(ta::sim::parseStartPositions(vfs_, tntPath).size());
            if (n > 0) players = std::to_string(n);
        }
        mapPreviewDims_ = size + (players.empty() ? "" : "   " + players + " PLAYER");
    }

    void GameView::buildGenPreview(const std::string& id) {
        // Roll the map from the seed (cheap integer-only work) and paint a downsampled
        // height/water map with feature + start markers, so the sliders preview live.
        ta::mapgen::Params gp = ta::mapgen::decodeMapId(id);
        ta::mapgen::Result g = ta::mapgen::generate(gp, vfs_);
        const ta::tnt::Map& m = g.map;
        const int W = m.width, H = m.height, sea = m.seaLevel;
        if (W <= 0 || H <= 0 || m.heights.size() < size_t(W) * H) return;
        const int cap = 192, mx = std::max(W, H);
        const int TW = std::max(1, W * cap / mx), TH = std::max(1, H * cap / mx);
        // World-flavoured land tint (loosely matches each world's ground section art).
        static const uint8_t landRGB[ta::mapgen::kMapTypes][3] = {
            {74, 118, 58}, {112, 84, 54}, {200, 172, 148}, {46, 92, 46}, {34, 80, 60}};
        const uint8_t* lc = landRGB[gp.mapType % ta::mapgen::kMapTypes];
        std::vector<uint8_t> rgba(size_t(TW) * TH * 4, 255);
        auto put = [&](int tx, int ty, uint8_t r, uint8_t gg, uint8_t b) {
            if (tx < 0 || ty < 0 || tx >= TW || ty >= TH) return;
            uint8_t* px = &rgba[(size_t(ty) * TW + tx) * 4];
            px[0] = r; px[1] = gg; px[2] = b; px[3] = 255;
        };
        // Base: height-shaded water (deeper -> darker) / land (brighter with elevation).
        for (int ty = 0; ty < TH; ++ty)
            for (int tx = 0; tx < TW; ++tx) {
                int h = m.heights[size_t(ty * H / TH) * W + (tx * W / TW)];
                if (h < sea) {
                    // Dark teal, matching the retail sea art the game actually draws.
                    int d = std::clamp((sea - h) * 2, 0, 40);
                    put(tx, ty, uint8_t(14 - d / 8), uint8_t(72 - d / 2), uint8_t(74 - d / 2));
                } else {
                    int e = std::clamp((h - sea) / 2, 0, 70);
                    put(tx, ty, uint8_t(std::min(255, lc[0] + e)),
                                uint8_t(std::min(255, lc[1] + e)), uint8_t(std::min(255, lc[2] + e)));
                }
            }
        // Feature dots -- sparse, so plot every one lest the downsample drop it. A
        // Sacred Stone (the mana spot) gets a bold 2x2 gold marker; Standing Stones
        // (ruins) are muted; rocks grey; trees dark green.
        for (int cz = 0; cz < H; ++cz)
            for (int cx = 0; cx < W; ++cx) {
                uint16_t fi = m.features[size_t(cz) * W + cx];
                if (fi == 0xFFFF || fi >= m.featureNames.size()) continue;
                const std::string& nm = m.featureNames[fi];
                int tx = cx * TW / W, ty = cz * TH / H;
                if (nm.find("Mana") != std::string::npos) {                                 // sacred spot: gold
                    for (int dy = 0; dy <= 1; ++dy)
                        for (int dx = 0; dx <= 1; ++dx) put(tx + dx, ty + dy, 250, 224, 82);
                } else if (nm.find("Henge") != std::string::npos) put(tx, ty, 176, 162, 132);  // ruins: stone
                else if (nm.find("Rock") != std::string::npos) put(tx, ty, 150, 148, 140);      // rock: grey
                else put(tx, ty, 28, 66, 28);                                                   // tree: dark green
            }
        // Start positions: bright 3x3 markers.
        for (auto& [sx, sz] : g.starts) {
            int tx = sx * TW / W, ty = sz * TH / H;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) put(tx + dx, ty + dy, 250, 250, 255);
        }
        mapPreviewTex_ = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, TW, TH);
        if (!mapPreviewTex_) return;
        SDL_SetTextureBlendMode(mapPreviewTex_, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(mapPreviewTex_, nullptr, rgba.data(), TW * 4);
        SDL_SetTextureScaleMode(mapPreviewTex_, SDL_ScaleModeNearest);
        mapPreviewW_ = TW; mapPreviewH_ = TH;
        char dims[48];
        std::snprintf(dims, sizeof dims, "%d x %d   %d PLAYER", W / 32, H / 32, int(gp.players));
        mapPreviewDims_ = dims;
    }

    void GameView::applyGenParams() {
        genParams_ = ta::mapgen::sanitize(genParams_);
        mpMapId_ = ta::mapgen::encodeMapId(genParams_);
        mapPath_ = mpMapId_;   // generated: the id IS the path (findMap returns it as-is)
    }

    void GameView::setGenSlider(int i, float mx) {
        const SDL_FRect& r = genSliderRect_[i];
        if (r.w <= 0) return;
        float t = std::clamp((mx - r.x) / r.w, 0.0f, 1.0f);
        uint8_t v = uint8_t(t * 255.0f + 0.5f);
        switch (i) {
            case 0: genParams_.treeDensity = v; break;
            case 1: genParams_.rockDensity = v; break;
            case 2: genParams_.manaDensity = v; break;
            case 3: genParams_.waterDensity = v; break;
            default: genParams_.reliefDensity = v; break;
        }
        applyGenParams();
    }

    void GameView::drawCreate(int winW, int winH) {
        (void)winW; (void)winH;
        float x = 80, y = 90;
        blockText(singlePlayer_ ? "SINGLE PLAYER VS AI" : "CREATE GAME", x, y, 2.2f,
                  {200, 205, 220, 255}); y += 40;
        // A private single-player game needs no name or password.
        if (!singlePlayer_) {
            lbField(x, y, 260, "GAME NAME", createName_, 1); y += 46;
            lbField(x, y, 260, "PASSWORD (optional)", createPass_, 2); y += 46;
        }
        blockText(std::string("MAP: ") + mapDisplayName(mpMapId_), x, y, 1.8f, {180, 185, 195, 255}); y += 30;
        // When OFF, losing your Monarch loses the game (retail commander rule); ON
        // makes the Monarch just another unit.
        lbBtn(x, y, 240, 26, createMonarchExp_ ? "MONARCH EXPENDABLE: ON"
                                               : "MONARCH EXPENDABLE: OFF", true,
              [this] { createMonarchExp_ = !createMonarchExp_; }); y += 34;
        // SP only: spectate mode -- you take no slot and just watch the AIs fight.
        // Seat AIs in the slots below, then START.
        if (singlePlayer_) {
            lbBtn(x, y, 240, 26, spSpectate_ ? "SPECTATE (WATCH AIS): ON"
                                             : "SPECTATE (WATCH AIS): OFF", true,
                  [this] { spSpectate_ = !spSpectate_; }); y += 34;
            // Stress test (spectate only): every AI starts at ~95% of the unit cap in
            // its faction's combat units -- an instant heavy load to profile the sim.
            if (spSpectate_) {
                lbBtn(x, y, 240, 26, createStressTest_ ? "STRESS TEST: ON"
                                                       : "STRESS TEST: OFF", true,
                      [this] { createStressTest_ = !createStressTest_; }); y += 34;
            }
        }
        // Override tier for the game: NONE (pure retail) / COSMETIC (art & sound
        // may differ) / FULL (gameplay overrides allowed but every player must
        // have the same ones). The host's own launch tier caps it (you can't offer
        // FULL if you didn't mount your gameplay overrides).
        // Fog of war: a room rule, so it is picked here and not only after the
        // room exists. Display-only (never hashed) -- every peer applies the same
        // rule, so it stays fair. Same three states the room button cycles.
        {
            static const char* kFogName[3] = {"NOT EXPLORED", "EXPLORED", "FULL VISION"};
            lbBtn(x, y, 240, 26,
                  std::string("FOG OF WAR: ") + kFogName[std::min<int>(createFog_, 2)], true,
                  [this] { createFog_ = uint8_t((createFog_ + 1) % 3); });
            y += 34;
        }
        static const char* kTier[] = {"NONE", "COSMETIC", "FULL"};
        lbBtn(x, y, 240, 26, std::string("OVERRIDES: ") + kTier[createOverride_ & 3], true,
              [this] { createOverride_ = uint8_t((createOverride_ + 1) % 3); }); y += 44;
        // Footer buttons, symmetric: BACK at the bottom-left and CREATE at the
        // bottom-right, both inset by `x` from their side and at the same height.
        // BROWSER (MP only) sits just left of CREATE.
        const float by = kLobbyH - 40, bw = 120;
        lbBtn(kLobbyW - x - bw, by, bw, 30, "CREATE", !createName_.empty(), [this] {
            ta::net::GameOptions o;
            o.overridePolicy = createOverride_;
            o.monarchExpendable = createMonarchExp_ ? 1 : 0;
            o.fogExplored = std::min<uint8_t>(createFog_, 2);
            // Stress test only applies to an all-AI spectate game.
            o.stressTest = (singlePlayer_ && spSpectate_ && createStressTest_) ? 1 : 0;
            // SP spectate: create as a spectator (no slot) so every slot can be an AI;
            // the game-start path flags spectating_ + noFog_ from mp_->isSpectator().
            mp_->createGame(createName_, createPass_, mpMapId_, o, mpCapacity(),
                            singlePlayer_ && spSpectate_, singlePlayer_);
            specAutoSeated_ = false;   // arm the one-shot auto-seat for this new room
            lobbyScreen_ = LobbyScreen::Browser;
        });
        if (!singlePlayer_)   // a private single-player game has no browser to go back to
            lbBtn(kLobbyW - x - bw - 122, by, 110, 30, "BROWSER", true,
                  [this] { lobbyScreen_ = LobbyScreen::Browser; });
        lbBtn(x, by, bw, 30, "BACK", true, [this] { menuRequested_ = true; });

        // Map picker (right column): a scrollable list box. Selecting sets both the
        // wire id (mpMapId_ = bare .tnt stem) and mapPath_, so mpCapacity() recomputes
        // the chosen map's start-position count.
        if (mapList_.empty()) buildMapList();
        // Adopt the remembered map once (persisted across launches), and scroll to it.
        if (!mapPrefApplied_ && settings_ && !settings_->lastMap.empty()) {
            for (int i = 0; i < int(mapList_.size()); ++i)
                if (mapList_[size_t(i)].name == settings_->lastMap) {
                    mpMapId_ = mapList_[size_t(i)].name;
                    mapPath_ = mapList_[size_t(i)].path;
                    mapScroll_ = std::max(0, i - 3);
                    break;
                }
            mapPrefApplied_ = true;
        }
        float lx = 400, hy = 90;
        blockText("SELECT MAP", lx, hy, 1.8f, {200, 205, 220, 255});
        // Toggle between the map list and the random-map generator.
        bool gen = ta::mapgen::isGeneratedMapId(mpMapId_);
        lbBtn(lx + 176, hy - 2, 164, 22, gen ? "PICK AN EXISTING MAP" : "GENERATE RANDOM MAP", true,
              [this, gen] {
                  if (gen) { mpMapId_.clear(); mapPath_.clear(); }  // drop back to the list
                  else applyGenParams();                           // encode the current gen params
              });
      if (!gen) {
        // Sort buttons: NAME / PLAYERS / SIZE. Clicking sets the sort key; clicking the
        // active one flips ascending/descending. The active key shows an up/down arrow.
        {
            const char* keys[3] = {"NAME", "PLAYERS", "SIZE"};
            const float bw = 86, bh = 20, sy = hy + 20;
            for (int k = 0; k < 3; ++k) {
                float bx = lx + k * (bw + 6);
                SDL_FRect b{bx, sy, bw, bh};
                bool active = mapSort_ == k;
                SDL_SetRenderDrawColor(ren_, active ? 56 : 34, active ? 74 : 38, active ? 96 : 50, 255);
                SDL_RenderFillRectF(ren_, &b);
                SDL_SetRenderDrawColor(ren_, 90, 100, 130, 255); SDL_RenderDrawRectF(ren_, &b);
                std::string lbl = keys[k];
                if (active) lbl += mapSortDir_ > 0 ? " ^" : " v";
                blockText(lbl, bx + 6, sy + 5, 1.5f,
                          active ? SDL_Color{215, 230, 245, 255} : SDL_Color{170, 178, 195, 255});
                lobbyHots_.push_back({b, [this, k] {
                    if (mapSort_ == k) mapSortDir_ = -mapSortDir_;   // toggle direction
                    else { mapSort_ = k; mapSortDir_ = 1; }
                    sortMapList();
                    mapScroll_ = 0;
                }});
            }
        }
        const float boxX = lx, boxY = hy + 46, boxW = 340, boxH = 344, rowH = 24, sbW = 12;
        const int total = int(mapList_.size());
        mapVisRows_ = int(boxH / rowH);
        mapTotalRows_ = total;
        clampMapScroll();
        mapListRect_ = {boxX, boxY, boxW, boxH};
        // box background + border
        SDL_FRect box{boxX, boxY, boxW, boxH};
        SDL_SetRenderDrawColor(ren_, 24, 26, 34, 255); SDL_RenderFillRectF(ren_, &box);
        SDL_SetRenderDrawColor(ren_, 70, 76, 96, 255); SDL_RenderDrawRectF(ren_, &box);
        const float rowW = boxW - sbW - 4;
        const float infoW = 96;                     // right-side "2P 8x8" column
        const int maxCh = int((rowW - infoW - 12) / 12);   // name chars that fit at px 2.0
        for (int r = 0; r < mapVisRows_; ++r) {
            int i = mapScroll_ + r;
            if (i >= total) break;
            const MapInfo& m = mapList_[size_t(i)];
            const std::string& nm = m.name;
            const std::string& pth = m.path;
            bool sel = (nm == mpMapId_);
            SDL_FRect row{boxX + 2, boxY + r * rowH, rowW, rowH};
            bool hot = lbHot(row);
            SDL_Color rc = sel ? SDL_Color{44, 78, 44, 255}
                         : hot ? SDL_Color{40, 46, 62, 255} : SDL_Color{24, 26, 34, 255};
            SDL_SetRenderDrawColor(ren_, rc.r, rc.g, rc.b, 255); SDL_RenderFillRectF(ren_, &row);
            blockText(nm.size() > size_t(maxCh) ? nm.substr(0, size_t(maxCh)) : nm,
                      boxX + 8, row.y + (rowH - 14) / 2, 2.0f,
                      sel ? SDL_Color{200, 240, 200, 255} : SDL_Color{220, 225, 235, 255});
            // Right-aligned player count + size, e.g. "4P 16x16".
            char info[24];
            if (m.sizeW > 0) std::snprintf(info, sizeof info, "%dP %dx%d", m.players, m.sizeW, m.sizeH);
            else std::snprintf(info, sizeof info, "%dP", m.players);
            blockText(info, boxX + rowW - blockWidth(info, 1.5f) - 6, row.y + (rowH - 11) / 2, 1.5f,
                      sel ? SDL_Color{170, 210, 170, 255} : SDL_Color{150, 160, 178, 255});
            lobbyHots_.push_back({row, [this, nm, pth] {
                mpMapId_ = nm; mapPath_ = pth;
                if (settings_) { settings_->lastMap = nm; saveSettings(*settings_); }  // remember it
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

      } else {
        // ---- random-map params panel (replaces the list) --------------------------
        static const char* kTypeName[ta::mapgen::kMapTypes] = {"ARAMON", "TAROS", "VERUNA", "ZHON", "CREON"};
        static const int kSizes[] = {8, 12, 16, 20, 24};   // section-units (x32 cells) per side
        // One rhythm for the whole panel: every control is kRowH tall and every gap
        // is kGap. The rows used to step by 30, 30, 36 and then 44, so the buttons
        // sat tighter than the sliders and the run of sliders drifted out of line
        // with everything above it.
        const float kRowH = 24, kGap = 8, kBtnW = 300;
        const float kSliderH = 32;   // label (11) + 7 + bar (14)
        float px = lx, py = hy + 46;   // aligns with the map list's box top
        lbBtn(px, py, kBtnW, kRowH, std::string("TYPE:  ") + kTypeName[genParams_.mapType % ta::mapgen::kMapTypes],
              true, [this] { genParams_.mapType = uint8_t((genParams_.mapType + 1) % ta::mapgen::kMapTypes);
                             applyGenParams(); }); py += kRowH + kGap;
        int curU = genParams_.widthCells / 32;
        char szl[48]; std::snprintf(szl, sizeof szl, "SIZE:  %d x %d", curU, curU);
        lbBtn(px, py, kBtnW, kRowH, szl, true, [this] {
            int u = genParams_.widthCells / 32, ni = 0;
            for (int k = 0; k < 5; ++k) if (kSizes[k] == u) ni = (k + 1) % 5;
            genParams_.widthCells = genParams_.heightCells = uint16_t(kSizes[ni] * 32);
            applyGenParams();
        }); py += kRowH + kGap;
        char pl[32]; std::snprintf(pl, sizeof pl, "PLAYERS:  %d", int(genParams_.players));
        lbBtn(px, py, kBtnW, kRowH, pl, true, [this] {
            genParams_.players = uint8_t(genParams_.players >= 8 ? 2 : genParams_.players + 1);
            applyGenParams();
        }); py += kRowH + kGap * 2;   // a wider break between the pickers and the sliders
        auto slider = [&](int idx, const char* label, uint8_t val) {
            blockText(label, px, py, 1.6f, {180, 185, 195, 255});
            float bx = px, by = py + 18, bw = kBtnW, bh = 14;
            genSliderRect_[idx] = {bx, by, bw, bh};
            SDL_FRect bar{bx, by, bw, bh};
            SDL_SetRenderDrawColor(ren_, 30, 34, 46, 255); SDL_RenderFillRectF(ren_, &bar);
            float t = val / 255.0f;
            SDL_FRect fill{bx, by, bw * t, bh};
            SDL_SetRenderDrawColor(ren_, 70, 120, 90, 255); SDL_RenderFillRectF(ren_, &fill);
            SDL_FRect handle{bx + bw * t - 3, by - 2, 6, bh + 4};
            SDL_SetRenderDrawColor(ren_, 200, 220, 200, 255); SDL_RenderFillRectF(ren_, &handle);
            SDL_SetRenderDrawColor(ren_, 70, 76, 96, 255); SDL_RenderDrawRectF(ren_, &bar);
            char pc[8]; std::snprintf(pc, sizeof pc, "%d%%", int(val) * 100 / 255);
            blockText(pc, bx + bw + 8, py + 16, 1.5f, {160, 165, 180, 255});
            py += kSliderH + kGap;
        };
        slider(0, "TREES", genParams_.treeDensity);
        slider(1, "ROCKS", genParams_.rockDensity);
        slider(2, "MANA SPOTS", genParams_.manaDensity);
        slider(3, "WATER", genParams_.waterDensity);
        slider(4, "HILLS  (plateaus & ramps)", genParams_.reliefDensity);
        lbBtn(px, py, 140, kRowH, "RE-ROLL SEED", true, [this] {
            genParams_.seed = genParams_.seed * 6364136223846793005ULL + 1442695040888963407ULL;
            applyGenParams();
        });
      }

        // Preview panel, right of the list / params column. A real map shows its
        // embedded minimap; a generated map shows a live thumbnail rolled from its
        // seed (both go through buildMapPreview -> mapPreviewTex_).
        const float pvx = lx + 352, pvy = hy + 46, pvW = 192, pvH = 192;
        blockText("PREVIEW", pvx, hy, 1.8f, {200, 205, 220, 255});
        SDL_FRect pbox{pvx, pvy, pvW, pvH};
        SDL_SetRenderDrawColor(ren_, 18, 20, 28, 255); SDL_RenderFillRectF(ren_, &pbox);
        if (mapPreviewFor_ != mapPath_) buildMapPreview(mapPath_);
        if (mapPreviewTex_ && mapPreviewW_ > 0) {
            float sc = std::min(pvW / float(mapPreviewW_), pvH / float(mapPreviewH_));
            float iw = mapPreviewW_ * sc, ih = mapPreviewH_ * sc;
            SDL_FRect dst{pvx + (pvW - iw) / 2, pvy + (pvH - ih) / 2, iw, ih};
            SDL_RenderCopyF(ren_, mapPreviewTex_, nullptr, &dst);
        } else {
            blockText(gen ? "GENERATING" : "NO PREVIEW", pvx + 34, pvy + pvH / 2 - 7, 1.6f,
                      {120, 125, 140, 255});
        }
        SDL_SetRenderDrawColor(ren_, 70, 76, 96, 255); SDL_RenderDrawRectF(ren_, &pbox);
        if (!mapPreviewDims_.empty())
            blockText(mapPreviewDims_, pvx, pvy + pvH + 8, 1.6f, {160, 165, 180, 255});
    }

    void GameView::drawRoom(int winW, int winH) {
        const auto& room = mp_->room();
        float x = 40, y = 78;
        blockText(room.name, x, y, 2.2f, {210, 210, 220, 255});
        blockText(std::string("MAP  ") + mapDisplayName(room.mapId), x + winW - 320, y + 4, 1.8f, {180, 185, 195, 255});
        // Override tier for this game (joiners adopt it; FULL needs matching gameplay files).
        static const char* kTier[] = {"NONE", "COSMETIC", "FULL"};
        blockText(std::string("OVERRIDES  ") + kTier[room.opts.overridePolicy & 3],
                  x + winW - 320, y + 22, 1.5f, {150, 175, 150, 255});
        y += 34;
        bool host = (room.hostId == mp_->myClientId());
        // SP spectate ("watch the AIs"): the instant the host lands in the room, fill
        // every open capacity slot with an AI -- each on its own team/colour and a
        // RANDOM race -- so the watcher gets a full free-for-all with no hand-seating.
        // Closed slots (no start position on this map) are skipped. One-shot
        // (specAutoSeated_), so the host can still tweak/close slots afterwards.
        if (singlePlayer_ && spSpectate_ && host && room.mySlot < 0 && !specAutoSeated_) {
            std::mt19937 rng(uint32_t(SDL_GetTicks64()) ^ (room.id * 2654435761u));
            for (int i = 0; i < ta::net::kMaxSlots; ++i)
                if (room.slots[i].type == 0)   // open capacity slot
                    mp_->setSlot(i, 2, uint8_t(rng() % uint32_t(factionCount())), uint8_t(i), uint8_t(i), 1,
                                 aiLevelEnv());
            specAutoSeated_ = true;
        }
        // slot table
        const char* typeName[4] = {"OPEN", "HUMAN", "AI", "CLOSED"};
        for (int i = 0; i < ta::net::kMaxSlots; ++i) {
            const auto& s = room.slots[i];
            bool mine = (i == room.mySlot);
            SDL_FRect row{x, y, winW - 320.0f, 30};
            SDL_SetRenderDrawColor(ren_, mine ? 40 : 26, mine ? 48 : 30, mine ? 66 : 40, 255);
            SDL_RenderFillRectF(ren_, &row);
            char sn[8]; std::snprintf(sn, sizeof sn, "%d", i + 1);
            blockText(sn, x + 8, y + 8, 1.8f, {150, 155, 170, 255});
            // type (host may cycle open<->closed on empty slots)
            SDL_Color tcol = s.type == 1 ? SDL_Color{200, 230, 200, 255}
                           : s.type == 2 ? SDL_Color{230, 220, 150, 255}
                                         : SDL_Color{130, 135, 150, 255};
            blockText(typeName[s.type % 4], x + 34, y + 8, 1.6f, tcol);
            if (host && s.type != 1) {
                // Host cycles an empty slot OPEN -> AI -> CLOSED. AI opponents are
                // seated (and run) on the server; this is how you set up multi-AI
                // games (including single-player vs several AIs).
                SDL_FRect tb{x + 34, y + 6, 60, 18};
                lobbyHots_.push_back({tb, [this, i, t = s.type] {
                    uint8_t nt = t == 0 ? 2 : (t == 2 ? 3 : 0);
                    const auto& s2 = mpRoom().slots[i];
                    mp_->setSlot(i, nt, s2.faction, s2.color, s2.team, 0, s2.aiLevel); }});
            }
            if (s.type == 1) blockText(s.name, x + 100, y + 8, 1.8f, {225, 228, 236, 255});
            else if (s.type == 2) {
                blockText(s.name.empty() ? "Computer" : s.name, x + 100, y + 8, 1.6f,
                          {210, 200, 150, 255});
                // AI difficulty (host cycles PASSIVE -> EASY -> NORMAL -> HARD -> ABSURD).
                static const char* diffName[5] = {"PASSIVE", "EASY", "NORMAL", "HARD", "ABSURD"};
                static const SDL_Color diffCol[5] = {
                    {150, 180, 210, 255},   // passive  (calm blue)
                    {150, 200, 150, 255},   // easy     (green)
                    {210, 200, 150, 255},   // normal   (neutral gold)
                    {225, 150, 140, 255},   // hard     (red)
                    {230, 120, 220, 255}};  // absurd   (magenta)
                uint8_t lvl = s.aiLevel % 5;
                blockText(diffName[lvl], x + 178, y + 9, 1.4f, diffCol[lvl]);
                if (host) { SDL_FRect db{x + 176, y + 6, 92, 18};
                    lobbyHots_.push_back({db, [this, i] { const auto& s2 = mpRoom().slots[i];
                        mp_->setSlot(i, s2.type, s2.faction, s2.color, s2.team, s2.ready,
                                     uint8_t((s2.aiLevel + 1) % 5)); }}); }
            }
            // faction / color / team edit: your own row, or (host) any AI row.
            bool canEdit = mine || (host && s.type == 2);
            blockText(factionName(s.faction), x + 260, y + 8, 1.6f, {200, 205, 215, 255});
            if (canEdit) { SDL_FRect fb{x + 260, y + 6, 90, 18};
                lobbyHots_.push_back({fb, [this, i] { const auto& s2 = mpRoom().slots[i];
                    mp_->setSlot(i, s2.type, uint8_t((s2.faction + 1) % factionCount()),
                                 s2.color, s2.team, s2.ready, s2.aiLevel); }}); }
            colorSwatch(x + 360, y + 5, 20, s.color, canEdit ? std::function<void()>([this, i] {
                // Cycle to the next colour NOT already held by another used slot --
                // landing on a taken colour just blocked READY, which was a trap.
                const auto& r2 = mpRoom();
                const auto& s2 = r2.slots[i];
                uint8_t next = s2.color;
                for (int step = 1; step <= 10; ++step) {
                    uint8_t cand = uint8_t((s2.color + step) % 10);
                    bool taken = false;
                    for (int k = 0; k < ta::net::kMaxSlots; ++k)
                        if (k != i && (r2.slots[k].type == 1 || r2.slots[k].type == 2) &&
                            r2.slots[k].color == cand) { taken = true; break; }
                    if (!taken) { next = cand; break; }
                }
                mp_->setSlot(i, s2.type, s2.faction, next, s2.team, s2.ready, s2.aiLevel); }) : nullptr);
            char tm[8]; std::snprintf(tm, sizeof tm, "T%d", s.team + 1);
            blockText(tm, x + 392, y + 8, 1.8f, {200, 205, 215, 255});
            if (canEdit) { SDL_FRect teb{x + 392, y + 6, 34, 18};
                lobbyHots_.push_back({teb, [this, i] { const auto& s2 = mpRoom().slots[i];
                    mp_->setSlot(i, s2.type, s2.faction, s2.color, uint8_t((s2.team + 1) % ta::net::kMaxSlots), s2.ready, s2.aiLevel); }}); }
            if (s.type == 1 && !singlePlayer_) {   // SP: the player is always ready, no column
                SDL_Color rc = s.ready ? SDL_Color{130, 230, 140, 255} : SDL_Color{120, 125, 135, 255};
                blockText(s.ready ? "READY" : "NOT READY", x + 440, y + 8, 1.6f, rc);
            }
            // host kick button for other humans
            if (host && s.type == 1 && !mine) {
                lbBtn(x + row.w - 54, y + 3, 50, 22, "KICK", true, [this, i] { mp_->kick(i); });
            }
            y += 34;
        }
        y += 10;
        // controls
        float bx = x;
        if (singlePlayer_) {
            // SP: no READY button -- the player is always ready. Keep the slot marked
            // ready (once, self-limiting) so the host's START enables.
            if (room.mySlot >= 0 && !room.slots[room.mySlot].ready) {
                const auto& s = room.slots[room.mySlot];
                mp_->setSlot(room.mySlot, 1, s.faction, s.color, s.team, 1, s.aiLevel);
            }
        } else {
            bool iAmReady = room.mySlot >= 0 && room.slots[room.mySlot].ready;
            lbBtn(x, y, 130, 30, iAmReady ? "UNREADY" : "READY", room.mySlot >= 0, [this, iAmReady] {
                const auto& s = mpRoom().slots[mpRoom().mySlot];
                mp_->setSlot(mpRoom().mySlot, 1, s.faction, s.color, s.team, iAmReady ? 0 : 1, s.aiLevel); });
            bx = x + 142;
        }
        // start (host): enabled when >=2 used slots and all humans ready and colors unique
        bool canStart = host && startValid(room);
        lbBtn(bx, y, 130, 30, "START", canStart, [this] { mp_->startGame(); },
              {70, 110, 70, 255});
        lbBtn(bx + 142, y, 120, 30, "LEAVE", true, [this] {
            mp_->leaveGame(); lobbyScreen_ = LobbyScreen::Browser;
            mpReadied_ = false; mpStarted_ = false; specAutoSeated_ = false; });
        // The game starts at normal speed; the host can allow it to be changed
        // in-game, and the host's -/+ keys then re-cadence the match live.
        y += 40;
        if (host) {
            std::string ub = std::string("ALLOW SPEED CHANGE IN-GAME: ") + (room.opts.speedUnlock ? "ON" : "OFF");
            lbBtn(x, y, 420, 26, ub, true, [this] {
                auto o = mpRoom().opts; o.speedUnlock = o.speedUnlock ? 0 : 1;
                mp_->setGameOptions(o); });
        }
        // Per-player unit cap (host cycles 250/500/1000/2000/5000; everyone sees it).
        y += 34;
        char cb[40]; std::snprintf(cb, sizeof cb, "UNIT CAP  %d", int(room.opts.unitCap));
        blockText(cb, x, y + 6, 2.0f, {205, 210, 225, 255});
        if (host) {
            lbBtn(x + 220, y, 90, 26, "CHANGE", true, [this] {
                static const uint16_t seq[] = {250, 500, 1000, 2000, 5000};
                auto o = mpRoom().opts; int idx = 3;   // default 2000
                for (int k = 0; k < 5; ++k) if (seq[k] == o.unitCap) idx = k;
                o.unitCap = seq[(idx + 1) % 5];
                mp_->setGameOptions(o); });
        }
        // Fog of war (display-only rule, never hashed): NOT EXPLORED darkens seen
        // terrain again when it leaves sight; EXPLORED keeps it dimmed-but-visible;
        // FULL VISION removes fog entirely (whole map + every unit, all players).
        y += 34;
        {
            static const char* kFogName[3] = {"NOT EXPLORED", "EXPLORED", "FULL VISION"};
            std::string fb = std::string("FOG OF WAR: ") +
                             kFogName[std::min<int>(room.opts.fogExplored, 2)];
            if (host)
                lbBtn(x, y, 420, 26, fb, true, [this] {
                    auto o = mpRoom().opts;
                    o.fogExplored = uint8_t((o.fogExplored + 1) % 3);
                    mp_->setGameOptions(o); });
            else
                blockText(fb, x, y + 6, 2.0f, {205, 210, 225, 255});
        }
        y += 34;
        {
            // Retail's "Random Start Locations" room option: shuffle which map start
            // each slot gets, so spawns can't be memorised on a familiar map.
            std::string rb = std::string("START LOCATIONS: ") +
                             (room.opts.randomStarts ? "RANDOM" : "FIXED");
            if (host)
                lbBtn(x, y, 420, 26, rb, true, [this] {
                    auto o = mpRoom().opts;
                    o.randomStarts = o.randomStarts ? 0 : 1;
                    mp_->setGameOptions(o); });
            else
                blockText(rb, x, y + 6, 2.0f, {205, 210, 225, 255});
        }
        // chat panel on the right (multiplayer only -- there's no one to chat with in SP)
        if (!singlePlayer_) {
            float chx = winW - 300.0f, chy = 78, chw = 280;
            SDL_SetRenderDrawColor(ren_, 22, 24, 32, 255);
            SDL_FRect cp{chx, chy, chw, winH - 150.0f}; SDL_RenderFillRectF(ren_, &cp);
            blockText("CHAT", chx + 8, chy + 6, 1.8f, {160, 165, 180, 255});
            float ly = chy + cp.h - 20;
            for (auto it = chatLog_.rbegin(); it != chatLog_.rend() && ly > chy + 26; ++it) {
                std::string line = it->first + ": " + it->second;
                if (line.size() > 40) line = line.substr(0, 40);
                blockText(line, chx + 8, ly, 1.4f, {200, 205, 215, 255});
                ly -= 16;
            }
            lbField(chx, winH - 66.0f, chw - 70, "SAY", chatDraft_, 4);
            lbBtn(chx + chw - 62, winH - 52.0f, 56, 24, "SEND", !chatDraft_.empty(), [this] {
                mp_->chat(chatDraft_); chatDraft_.clear(); });
        }
    }

    void GameView::lobbyMouse(float& mx, float& my) const {
        mx = mouseX_ / lobbyScale_ - lobbyOffX_;
        my = mouseY_ / lobbyScale_ - lobbyOffY_;
    }

    void GameView::lobbyInput(const SDL_Event& e, int winW, int winH) {
        (void)winW; (void)winH;
        if (e.type == SDL_MOUSEMOTION) {
            mouseX_ = float(e.motion.x); mouseY_ = float(e.motion.y);
            if (mapDrag_) setMapScrollFromThumb();   // dragging the map scrollbar
            if (genSlider_ >= 0) { float mx, my; lobbyMouse(mx, my); setGenSlider(genSlider_, mx); }
        } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
            mapDrag_ = false; genSlider_ = -1;
        } else if (e.type == SDL_MOUSEWHEEL) {
            float mx, my; lobbyMouse(mx, my);
            if (ptIn(mapListRect_, mx, my)) {   // scroll the map list under the cursor
                mapScroll_ -= e.wheel.y;
                clampMapScroll();
            }
        } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            mouseX_ = float(e.button.x); mouseY_ = float(e.button.y);
            lbField_ = 0; SDL_StopTextInput();
            float mx, my; lobbyMouse(mx, my);
            if (ptIn(mapThumbRect_, mx, my)) { mapDrag_ = true; return; }   // grab the thumb
            for (int gi = 0; gi < 5; ++gi)   // grab a density slider
                if (ptIn(genSliderRect_[gi], mx, my)) { genSlider_ = gi; setGenSlider(gi, mx); return; }
            for (auto& [r, action] : lobbyHots_)
                if (ptIn(r, mx, my)) { action(); break; }
        } else if (e.type == SDL_TEXTINPUT && lbField_) {
            std::string* f = lbFieldBuf();
            if (f && f->size() < 24) *f += e.text.text;
        } else if (e.type == SDL_KEYDOWN && lbField_) {
            if (e.key.keysym.sym == SDLK_BACKSPACE) { std::string* f = lbFieldBuf(); if (f && !f->empty()) f->pop_back(); }
            else if (e.key.keysym.sym == SDLK_RETURN) {
                if (lbField_ == 4 && !chatDraft_.empty()) { mp_->chat(chatDraft_); chatDraft_.clear(); }
                lbField_ = 0; SDL_StopTextInput();
            } else if (e.key.keysym.sym == SDLK_ESCAPE) { lbField_ = 0; SDL_StopTextInput(); }
        }
    }

