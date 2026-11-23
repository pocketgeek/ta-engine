#include "util/virtualpath.h"
#include "client/mapfilter.h"
#include "client/gameview.h"
#include "net/crypto.h"
#include "net/matchconfig.h"
#include "client/startmarkers.h"
#include "terrain/minimap.h"
#include <cctype>
#include <set>
#include <cstring>
#include <cmath>
#include "util/u8path.h"

// Out-of-line GameView method definitions (lobby concern), split from the
// class body in gameview.h so editing a body recompiles only this translation
// unit. Trivial getters, ctors, static, template, constexpr and default-arg
// methods stay inline in the header. Grouping is by name heuristic.

namespace {
// "<player>'s Game": the name the CREATE window offers, built as the server builds it for a
// game created without one (printable ASCII only, cut to net::kRoomNameMax bytes).
std::string defaultGameName(const std::string& player) {
    std::string name;
    for (unsigned char ch : player) if (ch >= 0x20 && ch <= 0x7e) name += char(ch);
    if (name.empty()) name = "Player";
    name += "'s Game";
    if (name.size() > tak::net::kRoomNameMax) name.resize(tak::net::kRoomNameMax);
    return name;
}
}  // namespace

    bool GameView::inLobbyPhase() const {
        if (!mp_ || mpSetupDone_) return false;
        auto s = mp_->state();
        return s == tak::net::MpClient::State::Connecting ||
               s == tak::net::MpClient::State::Lobby ||
               s == tak::net::MpClient::State::InRoom ||
               s == tak::net::MpClient::State::Done;
    }

    int GameView::geomSlot(int id) const {
        const int slot=front().slot(id);
        return slot>=0 && size_t(slot)<geomIndex_.size() ? geomIndex_[size_t(slot)] : -1;
    }

    void GameView::drawLobby(int winW, int winH) {
        pollGenPreview();   // a random map's preview rolled on its worker
        lobbyHots_.clear();
        // Map-picker geometry is only live while the CHANGE MAP panel is drawn; clear it
        // so a stale thumb/list rect can't grab clicks or wheel on the other screens.
        mapListRect_ = mapThumbRect_ = chatRect_ = SDL_FRect{0, 0, 0, 0};
        for (auto& rect : genSliderRect_) rect = {};
        // absorb any new chat
        if (mp_) for (auto& m : mp_->takeChat()) chatLog_.push_back(m);
        // The ground + centred panel frame are painted by the caller (the viewport is
        // already offset to this panel); RenderClear would ignore the viewport.
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        float cx = winW / 2.0f;
        // Titled as the door that opens it (PlayComputer's and PlayPlayer's captions).
        const char* title = modsScreen_ && mp_ && mp_->state() == tak::net::MpClient::State::InRoom ? "MODS"
                          : singlePlayer_ ? "PLAY THE MACHINE" : "PLAY AN OPPONENT";
        blockText(title, cx - blockWidth(title, 2.6f) / 2, 24, 2.6f, {210, 200, 150, 255});
        if (!mp_) return;
        auto st = mp_->state();
        if (st == tak::net::MpClient::State::Connecting) {
            blockText("CONNECTING TO SERVER...", cx - 120, winH / 2.0f, 2.0f, {200, 200, 210, 255});
            return;
        }
        if (st == tak::net::MpClient::State::Done) {
            // Connection over (server closed / rejected us / timed out): say WHY --
            // MpClient preserves the server's Reject reason (e.g. "protocol version
            // mismatch (server 28 / client 29)") -- and give the player a way OUT.
            float my = winH / 2.0f - 40;
            std::string why = !lanJoinError_.empty() ? lanJoinError_ : lanHostGone() ? std::string("the host closed the game")
                            : mp_->error().empty() ? std::string("the server closed the connection")
                                                   : mp_->error();
            const char* head = !lanJoinError_.empty() ? "CANNOT JOIN"
                             : why == "the host closed the game" ? "GAME CLOSED" : "DISCONNECTED";
            blockText(head, cx - blockWidth(head, 2.6f) / 2, my, 2.6f, {255, 130, 110, 255});
            // Long reasons (version-mismatch text) wrap onto two centred lines.
            auto rows = tak::uiTextWrap(why, 1.8f, 600);
            const std::string l1 = rows[0];
            std::string l2;
            for (size_t k = 1; k < rows.size(); ++k) l2 += (k > 1 ? " " : "") + rows[k];
            l2 = tak::uiTextFit(l2, 1.8f, 600);
            blockText(l1, cx - blockWidth(l1, 1.8f) / 2, my + 34, 1.8f, {215, 218, 228, 255});
            if (!l2.empty())
                blockText(l2, cx - blockWidth(l2, 1.8f) / 2, my + 56, 1.8f, {215, 218, 228, 255});
            // A LAN game's BACK returns to the CONNECT screen (main reopens it).
            lbBtn(cx - 90, my + (l2.empty() ? 78.0f : 96.0f), 180, 32, mp_->lanServer() ? "BACK" : "BACK TO MENU", true,
                  [this] { menuRequested_ = true; });
            return;
        }
        // Someone else's LOCAL SERVER: into its open game, never the game list.
        if (st == tak::net::MpClient::State::Lobby && mp_->lanServer() && !lanHost_) {
            lanJoinStep();
            if (mp_->state() == tak::net::MpClient::State::Lobby) {
                const char* note = lanWaitingStart_ ? "WAITING FOR THE GAME TO START..." : "JOINING THE GAME...";
                blockText(note, cx - blockWidth(note, 2.0f) / 2, winH / 2.0f, 2.0f, {200, 200, 210, 255});
            }
            return;
        }
        if (st != tak::net::MpClient::State::InRoom) modsScreen_ = false;   // left the room: back to it next time
        if (st != tak::net::MpClient::State::InRoom) lanLeavePrompt_ = false;
        if (st != tak::net::MpClient::State::InRoom) closeMapPicker();   // the panel goes with the room
        if (st == tak::net::MpClient::State::InRoom) {
            roomStep();   // the room's reactions run while MODS is open too
            // Under the CHANGE MAP panel or the LAN leave prompt nothing of the room lights up.
            lobbyModal_ = picker_.open || lanLeavePrompt_;
            if (modsScreen_) drawModsScreen(winW, winH); else drawRoom(winW, winH);
            lobbyModal_ = false;
            if (picker_.open) drawMapPicker(winW, winH);   // the CHANGE MAP panel, over the room
            if (lanLeavePrompt_) drawLanLeavePrompt(winW, winH);
            if (!roomShotPath_.empty()) roomShotCapture();   // TAK_ROOM_SCRIPT shot:, as drawn
        }
        // A room-first session has no game list: its room is on its way, or could not open.
        else if (singlePlayer_ || lanHost_) drawRoomOpening(winW, winH);
        else {
            drawBrowser(winW, winH);
            if (!roomShotPath_.empty()) roomShotCapture();   // TAK_ROOM_SCRIPT shot:, as drawn
        }
    }

    void GameView::drawRoomOpening(int winW, int winH) {
        const float cx = winW / 2.0f, my = winH / 2.0f - 40;
        // A refused CreateGame (the server's reason), or a map that could not be built here.
        const std::string why = !mp_->joinError().empty()   ? mp_->joinError()
                              : !mp_->createError().empty() ? mp_->createError()
                                                            : roomOpenError_;
        if (roomAutoCreate_ || roomCreateSent_ || why.empty()) {
            const char* note = "OPENING THE GAME...";
            blockText(note, cx - blockWidth(note, 2.0f) / 2, my + 20, 2.0f, {200, 200, 210, 255});
        } else {
            const char* head = "CANNOT OPEN A GAME";
            blockText(head, cx - blockWidth(head, 2.6f) / 2, my, 2.6f, {255, 130, 110, 255});
            const auto rows = tak::uiTextWrap(why, 1.8f, 600);
            for (size_t k = 0; k < rows.size() && k < 2; ++k) {
                const std::string line = k == 1 && rows.size() > 2 ? tak::uiTextFit(rows[1] + " " + rows[2], 1.8f, 600) : rows[k];
                blockText(line, cx - blockWidth(line, 1.8f) / 2, my + 34 + 22 * float(k), 1.8f, {215, 218, 228, 255});
            }
        }
        // Always a way back, so a create that is never answered cannot strand the player. A
        // session from the command line (--play-map, the CLI game) has no menu to go back to.
        const char* back = !canReturnToMenu_ ? "QUIT" : lanHost_ ? "BACK" : "BACK TO MENU";
        lbBtn(cx - 90, my + 96, 180, 32, back, true, [this] { menuRequested_ = true; });
    }

    // The other humans seated in the room: who a LOCAL SERVER host's LEAVE turns out.
    int GameView::lanOthersInRoom() const {
        const auto& room = mp_->room();
        int n = 0;
        for (int i = 0; i < tak::net::kMaxSlots; ++i) n += i != room.mySlot && room.slots[i].type == 1;
        return n;
    }

    void GameView::leaveRoom() {
        mp_->leaveGame();
        mpReadied_ = false; mpStarted_ = false;
        lanLeavePrompt_ = false;
        tak::crypto::wipe(roomPass_);   // the room's password goes with the room
        roomPassShown_ = false;
        // Whatever room comes next (this one again too) is seen afresh: nothing it shows
        // first is a change.
        roomSeenSeq_ = 0;
        // The CHANGE MAP panel goes, and its map list is read again in the next room: maps
        // received in this one appear there.
        closeMapPicker(); picker_.pickSerial = 0;
        mapListScanned_ = false;
    }

    void GameView::roomLeave() {
        leaveRoom();
        if (singlePlayer_ || lanHost_) menuRequested_ = true;
    }

    // LEAVE WILL CLOSE THE GAME FOR N PLAYERS: YES leaves (the server closes the room for
    // everyone), NO stays. Over the room, which takes no clicks while it is open.
    void GameView::drawLanLeavePrompt(int winW, int winH) {
        lobbyHots_.clear();
        SDL_FRect dim{0, 0, float(winW), float(winH)};
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 160); SDL_RenderFillRectF(ren_, &dim);
        const float pw = 520, ph = 150, px = (winW - pw) / 2, py = (winH - ph) / 2;
        SDL_FRect panel{px, py, pw, ph};
        SDL_SetRenderDrawColor(ren_, 28, 30, 40, 250); SDL_RenderFillRectF(ren_, &panel);
        SDL_SetRenderDrawColor(ren_, 190, 160, 110, 255); SDL_RenderDrawRectF(ren_, &panel);
        const int n = lanOthersInRoom();
        const std::string line = "LEAVE WILL CLOSE THE GAME FOR " + std::to_string(n) + (n == 1 ? " PLAYER." : " PLAYERS.");
        blockText(line, px + (pw - blockWidth(line, 1.8f)) / 2, py + 30, 1.8f, {225, 215, 170, 255});
        blockText("LEAVE?", px + (pw - blockWidth("LEAVE?", 1.8f)) / 2, py + 58, 1.8f, {225, 215, 170, 255});
        lbBtn(px + pw / 2 - 140, py + ph - 50, 120, 30, "YES", true, [this] { roomLeave(); });
        lbBtn(px + pw / 2 + 20, py + ph - 50, 120, 30, "NO", true, [this] { lanLeavePrompt_ = false; });
    }

    void GameView::lanJoinStep() {
        const uint64_t now = SDL_GetTicks64();
        auto fail = [&](std::string why) { lanJoinError_ = std::move(why); mp_->disconnect(lanJoinError_); };
        if (lanJoinSent_) {   // asked: in the room by now, or the server said why not
            if (!mp_->joinError().empty()) fail(mp_->joinError());
            return;
        }
        if (!lanJoinAtMs_) lanJoinAtMs_ = now;
        if (now - mpListMs_ > 1000) { mp_->listGames(); mpListMs_ = now; }
        bool started = false;
        lanWaitingStart_ = false;
        for (const auto& g : mp_->games()) {
            if (lanSpectate_) {   // watching: a started game now, a waiting one from its start
                if (!g.running) { lanWaitingStart_ = true; continue; }
                roomKind_ = RoomKind::Multiplayer; rememberRoom_ = lobbyAutoMode_ == 0;
                mp_->spectate(g.id, joinPass_);
                tak::crypto::wipe(joinPass_);
                lanJoinSent_ = true;
                return;
            }
            if (g.running) { started = true; continue; }
            roomKind_ = RoomKind::Multiplayer; rememberRoom_ = lobbyAutoMode_ == 0;
            mp_->joinGame(g.id, joinPass_);
            roomPassAsked_ = joinPass_;   // the room shows it behind SHOW / HIDE (roomPasswordStep)
            tak::crypto::wipe(joinPass_);
            lanJoinSent_ = true;
            return;
        }
        if (lanWaitingStart_) return;
        if (started) fail("the game on this server has already started -- SPECTATE to watch it");
        else if (now - lanJoinAtMs_ > 5000) fail("the host has not opened a game on this server yet");
    }

    void GameView::drawBrowser(int winW, int winH) {
        // keep the list fresh without needing a manual refresh
        if (SDL_GetTicks64() - mpListMs_ > 1000) { mp_->listGames(); mpListMs_ = SDL_GetTicks64(); }
        // Under the CREATE window or a password window nothing of the list lights up.
        const bool window = browserWin_.kind != BrowserWindow::Kind::None;
        lobbyModal_ = window;
        float x = 60, y = 80, w = winW - 120.0f;
        blockText("GAMES", x, y, 2.2f, {200, 205, 220, 255});
        lbBtn(x + w - 200, y - 6, 95, 26, "REFRESH", true, [this] { mp_->listGames(); });
        // CREATE asks the game's name and password (its window), then opens the room from
        // the multiplayer remembered settings.
        lbBtn(x + w - 100, y - 6, 100, 26, "CREATE", true, [this] { openCreateWindow(); });
        y += 34;
        // column header (the NAME column holds "<a 16-character name>'s Game" whole)
        constexpr float kNameW = 264, kMapX = 280;
        blockText("NAME", x + 8, y, 1.6f, {130, 135, 150, 255});
        blockText("MAP", x + kMapX, y, 1.6f, {130, 135, 150, 255});
        blockText("PLAYERS", x + w - 200, y, 1.6f, {130, 135, 150, 255});
        y += 20;
        const auto& games = mp_->games();
        if (games.empty())
            blockText("NO GAMES -- CREATE ONE, OR REFRESH", x + 8, y + 10, 1.8f, {150, 150, 160, 255});
        const bool loading = browserWatchLoading();   // a WATCH is on its way in: no other until it is, or is taken back
        for (const auto& g : games) {
            SDL_FRect row{x, y, w, 30};
            bool hot = lbHot(row) && !g.running;
            SDL_SetRenderDrawColor(ren_, hot ? 46 : 30, hot ? 52 : 34, hot ? 72 : 44, 255);
            SDL_RenderFillRectF(ren_, &row);
            // A name is up to 32 bytes: shrunk (on its baseline) before it is cut.
            const float namePx = tak::uiTextFitPx(g.name, 1.8f, kNameW, 1.4f);
            blockText(tak::uiTextFit(g.name, namePx, kNameW), x + 8, y + 8 + 7 * (1.8f - namePx), namePx, {225, 228, 236, 255});
            blockText(tak::uiTextFit(mapTitle(g.mapId), 1.6f, w - 262 - kMapX), x + kMapX, y + 8, 1.6f, {180, 185, 195, 255});
            char pc[32]; std::snprintf(pc, sizeof pc, "%d/%d", g.players, g.capacity);
            blockText(pc, x + w - 200, y + 8, 1.8f, {200, 205, 215, 255});
            if (g.passworded)   // (a live game's: left of its player count, clear of LIVE and WATCH)
                blockText("LOCK", x + w - (g.running ? 252 : 120), y + 8, 1.6f, {230, 200, 120, 255});
            if (g.running) {
                // A running game can't be joined, but it can be watched live -- unless
                // its host turned ALLOW SPECTATORS off. Then the button says so (muted
                // red) and a click still asks: the server is the authority, and its
                // refusal puts the reason on the CANNOT JOIN line below.
                blockText("LIVE", x + w - 158, y + 8, 1.6f, {230, 140, 120, 255});
                lbBtn(x + w - 96, y + 3, 92, 24, g.allowSpectators ? "WATCH" : "NO WATCH", !loading,
                      [this, id = g.id] { browserGameClick(id, true); },
                      g.allowSpectators ? SDL_Color{60, 66, 86, 255} : SDL_Color{86, 52, 52, 255});
            } else {
                lobbyHots_.push_back({row, [this, id = g.id] { browserGameClick(id, false); }});
            }
            y += 34;
        }
        // The bottom line: a WATCH that is loading says what is arriving, with CANCEL to take
        // it back (a locked game's window says and does the same); else the last refusal.
        if (const std::string loading = window ? std::string() : browserLoading(); !loading.empty()) {
            blockText(tak::uiTextFit(loading, 1.6f, winW - 2 * x - 270), x, winH - 31.0f, 1.6f, {200, 200, 210, 255});
            lbBtn(winW - 270.0f, winH - 40.0f, 120, 30, "CANCEL", true, [this] { browserTakeBack(); });
        } else if (const std::string why = browserLine(); !why.empty())
            blockText(tak::uiTextFit(why, 1.6f, winW - 2 * x - 140), x, winH - 31.0f, 1.6f, {255, 130, 110, 255});
        lbBtn(winW - 140.0f, winH - 40.0f, 120, 30, "BACK", true, [this] { menuRequested_ = true; });
        lobbyModal_ = false;
        if (window) drawBrowserWindow(winW, winH);
    }

    // Why the server turned the last JOIN / WATCH away (game full, spectators not
    // allowed...), in its own case (as the CONNECT dialog shows server text) -- until the
    // next request clears it. A refusal a window showed (a wrong password) is not repeated
    // here. A request that got no answer says so too, unless a window is there to say it
    // (a WATCH whose game ended while it was loading: that). One that CANCEL took back has
    // nothing to say: its answer is dropped (browserTakeBack).
    std::string GameView::browserLine() const {
        if (!mp_->joinError().empty() && !browserLineQuiet_) return "CANNOT JOIN: " + mp_->joinError();
        const bool window = browserWin_.kind != BrowserWindow::Kind::None;
        if (!roomRequestOut() || browserBusy() || window) return "";
        return roomJoinSent_ && mp_->watchLoading() ? "THE GAME HAS CLOSED" : "NO ANSWER FROM THE SERVER";
    }

    // The WATCH the browser waits on has its answer: the server took it (GameStarting came,
    // and waits in MpClient for the game's map and game mods to arrive and verify), so no
    // clock runs on it. Unless its game has left the list: the server answers no list while
    // it has this client in a game, so that game has ended and nothing more of it is coming.
    bool GameView::browserWatchLoading() const {
        if (!roomJoinSent_ || !browserWatchId_ || !mp_->watchLoading()) return false;
        const auto& games = mp_->games();
        return std::any_of(games.begin(), games.end(), [&](const tak::net::GameInfo& g) { return g.id == browserWatchId_; });
    }

    std::string GameView::browserLoading() const {
        if (!browserWatchLoading()) return {};
        // The package still on its way, the map first (or why one could not be used).
        const std::string& map = mp_->mapStatus();
        if (!map.empty() && map != "MAP READY" && map.rfind("MAP VERIFIED", 0) != 0) return map;
        const std::string& mods = mp_->modStatus();
        if (!mods.empty() && mods.rfind("GAME MODS VERIFIED", 0) != 0) return mods;
        return "LOADING THE GAME...";
    }

    // The request the browser waits on is let go of: CANCEL took it back, or no answer came.
    // MpClient::leaveGame sends LeaveGame behind it (a room or watched game the server opens
    // for it is left there at once) and drops its answer, yes or no, when it arrives. The
    // password that went with it is wiped, and the next request may go out straight away.
    void GameView::browserTakeBack() {
        mp_->leaveGame();
        roomCreateSent_ = roomJoinSent_ = false;
        roomOpenNote_.clear();   // (CREATE's note for a room nobody saw is not the next room's)
        tak::crypto::wipe(roomPassAsked_);
    }

    bool GameView::browserRequestFree() {
        if (!roomRequestOut()) return true;
        if (browserBusy()) return false;
        browserTakeBack();   // no answer is coming
        return true;
    }

    bool GameView::browserJoin(uint32_t id, const std::string& password, bool watch) {
        if (!browserRequestFree()) { roomActError_ = "the server has not answered the last request yet"; return false; }
        roomKind_ = RoomKind::Multiplayer; rememberRoom_ = lobbyAutoMode_ == 0;
        if (watch) mp_->spectate(id, password);
        else mp_->joinGame(id, password);
        // A game joined shows its password in the room (roomPasswordStep); one watched has no room.
        tak::crypto::wipe(roomPassAsked_);
        if (!watch) roomPassAsked_ = password;
        roomJoinSent_ = true; roomRequestMs_ = SDL_GetTicks64();
        browserWatchId_ = watch ? id : 0;
        browserLineQuiet_ = false;
        return true;
    }

    // A locked game asks its password first. A live game with ALLOW SPECTATORS off is asked
    // for as it is: the server refuses it before it reads a password, and says why.
    void GameView::browserGameClick(uint32_t id, bool watch) {
        const auto& games = mp_->games();
        const auto game = std::find_if(games.begin(), games.end(), [&](const tak::net::GameInfo& g) { return g.id == id; });
        if (game == games.end()) { roomActError_ = "the game is no longer listed"; return; }
        if (game->passworded && (!watch || game->allowSpectators)) openPasswordWindow(*game, watch);
        else browserJoin(id, "", watch);
    }

    void GameView::openCreateWindow() {
        if (!mp_ || mp_->state() != tak::net::MpClient::State::Lobby) { roomActError_ = "not in the game list"; return; }
        closeBrowserWindow();
        auto& w = browserWin_;
        w.kind = BrowserWindow::Kind::Create;
        // "<account>'s Game" (on a server without accounts: the name this client plays under),
        // filled in, with the caret at its end.
        w.name = w.defaultName = defaultGameName(mp_->account().empty() ? localName_ : mp_->account());
        lbFocus(w.focus = 11);
    }

    void GameView::openPasswordWindow(const tak::net::GameInfo& game, bool watch) {
        closeBrowserWindow();
        auto& w = browserWin_;
        w.kind = watch ? BrowserWindow::Kind::Watch : BrowserWindow::Kind::Join;
        w.gameId = game.id; w.gameName = game.name;
        lbFocus(w.focus = 12);
    }

    void GameView::closeBrowserWindow() {
        auto& w = browserWin_;
        if (w.kind == BrowserWindow::Kind::None) return;
        if (lbField_ == 11 || lbField_ == 12) lbFocus(0);
        tak::crypto::wipe(w.password);
        w = {};
    }

    // CANCEL and Esc. While the window's own request (CREATE, JOIN or WATCH) is still
    // unanswered they take it back: a yes on its way opens no room and no game, and a no is
    // not news (browserTakeBack).
    void GameView::cancelBrowserWindow() {
        if (browserWin_.asked && roomRequestOut()) browserTakeBack();
        closeBrowserWindow();
    }

    const char* GameView::browserWindowGone() const {
        const auto& w = browserWin_;
        if (w.kind != BrowserWindow::Kind::Join && w.kind != BrowserWindow::Kind::Watch) return nullptr;
        for (const auto& g : mp_->games())
            if (g.id == w.gameId) return w.kind == BrowserWindow::Kind::Join && g.running ? "THE GAME HAS STARTED" : nullptr;
        return "THE GAME HAS CLOSED";
    }

    // No request still waiting for its answer; and for a password window, a password to send
    // and its game still there to ask into.
    bool GameView::browserWindowReady() const {
        const auto& w = browserWin_;
        if (w.kind == BrowserWindow::Kind::None || browserBusy()) return false;
        return w.kind == BrowserWindow::Kind::Create || (!w.password.empty() && !browserWindowGone());
    }

    void GameView::browserWindowCommit() {
        auto& w = browserWin_;
        if (!browserWindowReady()) {
            const char* gone = browserBusy() ? nullptr : browserWindowGone();
            roomActError_ = browserBusy() ? "the server has not answered the last request yet"
                          : gone ? gone : "a locked game needs its password";
            return;
        }
        w.error.clear();
        if (lbField_ == 11 || lbField_ == 12) w.focus = lbField_;
        if (w.kind == BrowserWindow::Kind::Create) {
            // The name without the spaces around it; a blank one creates the game under the
            // default name.
            roomStartName_ = w.name;
            roomStartName_.erase(0, roomStartName_.find_first_not_of(' '));
            roomStartName_.erase(roomStartName_.find_last_not_of(' ') + 1);
            if (roomStartName_.empty()) roomStartName_ = w.defaultName;
            roomStartPass_ = w.password;
            roomActError_.clear();
            if (browserRequestFree()) createRoomFromPrefs();
            w.asked = roomCreateSent_;
            // Nothing went out: no map could be built here.
            if (!w.asked) w.error = "CANNOT OPEN A GAME: " + roomActError_;
            browserLineQuiet_ = false;
        } else {
            w.asked = browserJoin(w.gameId, w.password, w.kind == BrowserWindow::Kind::Watch);
        }
        if (w.asked) lbFocus(0);   // the fields are greyed until the answer
    }

    void GameView::browserStep() {
        auto& w = browserWin_;
        if (mp_->state() != tak::net::MpClient::State::Lobby) {
            // In the room or the watched game, or the connection is over: the window goes.
            roomJoinSent_ = false;
            closeBrowserWindow();
            return;
        }
        if (!mp_->joinError().empty()) roomJoinSent_ = false;   // refused (mpAutoStep clears roomCreateSent_)
        if (w.kind == BrowserWindow::Kind::None) return;
        if (w.asked && !roomRequestOut()) {
            // Refused: the window says why, with the text there to change and send again.
            w.asked = false;
            w.error = (w.kind == BrowserWindow::Kind::Create ? "CANNOT OPEN A GAME: " : "CANNOT JOIN: ") + mp_->joinError();
            browserLineQuiet_ = true;
            lbFocus(w.focus);
        } else if (w.asked && !browserBusy() && w.error.empty()) {
            w.error = "NO ANSWER FROM THE SERVER";
            lbFocus(w.focus);
        }
        // A game that closed or started under its window takes no more typing.
        if (!(w.asked && browserBusy()) && browserWindowGone() && (lbField_ == 11 || lbField_ == 12)) lbFocus(0);
    }

    // The CREATE window, or a locked game's password window: over the game list, which is
    // dimmed and takes no clicks (the look of the LAN leave prompt).
    void GameView::drawBrowserWindow(int winW, int winH) {
        using Kind = BrowserWindow::Kind;
        const auto& w = browserWin_;
        lobbyHots_.clear();
        SDL_FRect dim{0, 0, float(winW), float(winH)};
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 160); SDL_RenderFillRectF(ren_, &dim);
        const bool create = w.kind == Kind::Create;
        const float pw = 520, ph = create ? 240 : 176, px = (winW - pw) / 2, py = (winH - ph) / 2;
        SDL_FRect panel{px, py, pw, ph};
        SDL_SetRenderDrawColor(ren_, 28, 30, 40, 250); SDL_RenderFillRectF(ren_, &panel);
        SDL_SetRenderDrawColor(ren_, 190, 160, 110, 255); SDL_RenderDrawRectF(ren_, &panel);
        const float fx = px + 24, fw = pw - 48;
        // While its own request waits for the answer the fields are greyed; so they are once
        // the game closed, or started under a JOIN.
        const bool waiting = w.asked && browserBusy();
        const char* gone = waiting ? nullptr : browserWindowGone();
        const bool typing = !waiting && !gone;
        const std::string head = create ? std::string("CREATE A GAME") : tak::uiTextFit("PASSWORD FOR " + w.gameName, 1.8f, fw);
        blockText(head, fx, py + 20, 1.8f, {225, 215, 170, 255});
        // A password field with its SHOW / HIDE beside it: one pip per character until SHOW.
        const auto passwordField = [&](float y, const char* label) {
            const float tw = 58;
            lbField(fx, y, fw - tw - 8, label, w.password, 12, typing, w.showPassword ? FieldText::Shown : FieldText::Hidden);
            lbBtn(fx + fw - tw, y + 14, tw, 22, w.showPassword ? "HIDE" : "SHOW", typing,
                  [this] { browserWin_.showPassword = !browserWin_.showPassword; }, {60, 66, 86, 255}, 1.5f);
        };
        float y = py + 36;
        if (create) {
            lbField(fx, y + 14, fw, "GAME NAME", w.name, 11, typing);
            passwordField(y + 62, "PASSWORD (OPTIONAL)");
            y += 110;
        } else {
            passwordField(y, "");
            y += 48;
        }
        // One line under the fields: the wait (for a WATCH the server took: the map or game
        // mods arriving), why the game can no longer be asked into, an earlier request the
        // server has yet to answer (a game's row clicked just before: why the button is
        // grey), or the last answer (a refusal in the server's own words; a long one takes
        // two rows).
        constexpr SDL_Color kWait{200, 200, 210, 255}, kWarn{255, 130, 110, 255};
        if (const std::string loading = waiting ? browserLoading() : std::string(); !loading.empty())
            blockText(tak::uiTextFit(loading, 1.6f, fw), fx, y, 1.6f, kWait);   // the watch was taken: what is arriving
        else if (waiting)
            blockText(create ? "OPENING THE GAME..." : w.kind == Kind::Watch ? "OPENING..." : "JOINING...", fx, y, 1.6f, kWait);
        else if (gone) blockText(gone, fx, y, 1.6f, kWarn);
        else if (browserBusy()) blockText("WAITING FOR THE SERVER...", fx, y, 1.6f, kWait);
        else if (!w.error.empty()) {
            auto rows = tak::uiTextWrap(w.error, 1.6f, fw);
            for (size_t k = 2; k < rows.size(); ++k) rows[1] += " " + rows[k];
            blockText(rows[0], fx, y, 1.6f, kWarn);
            if (rows.size() > 1) blockText(tak::uiTextFit(rows[1], 1.6f, fw), fx, y + 16, 1.6f, kWarn);
        }
        const float by = py + ph - 46;
        if (gone) {
            lbBtn(px + pw / 2 - 60, by, 120, 30, "CANCEL", true, [this] { cancelBrowserWindow(); });
        } else {
            // (Side by side, both labels on their caps: JOIN's J has a tail.)
            lbBtn(px + pw / 2 - 140, by, 120, 30, create ? "CREATE" : w.kind == Kind::Watch ? "WATCH" : "JOIN",
                  browserWindowReady(), [this] { browserWindowCommit(); }, {70, 110, 70, 255}, kBtnLabelPx, true);
            lbBtn(px + pw / 2 + 20, by, 120, 30, "CANCEL", true, [this] { cancelBrowserWindow(); }, {60, 66, 86, 255},
                  kBtnLabelPx, true);
        }
    }

    // Enter is the window's JOIN / WATCH / CREATE, Esc its CANCEL, Tab the CREATE window's
    // other field. A click reaches the window's own widgets only, and one that misses them
    // (or lands on SHOW / HIDE) leaves the caret where it is. (The rest of the double click
    // that opened the window never gets here: lobbyPressStale.)
    // Typing and Backspace go on to lobbyInput's fields.
    bool GameView::browserWindowInput(const SDL_Event& e) {
        if (e.type == SDL_KEYDOWN) {
            const auto key = e.key.keysym.sym;
            if (key != SDLK_ESCAPE && key != SDLK_RETURN && key != SDLK_KP_ENTER && key != SDLK_TAB) return false;
            if (e.key.repeat) return true;   // a held key acts once
            if (key == SDLK_ESCAPE) cancelBrowserWindow();
            else if (key == SDLK_TAB) {
                if (browserWin_.kind == BrowserWindow::Kind::Create && lbField_) lbFocus(lbField_ == 11 ? 12 : 11);
            } else if (browserWindowReady()) browserWindowCommit();
            return true;
        }
        if (e.type == SDL_MOUSEBUTTONDOWN) {
            mouseX_ = float(e.button.x); mouseY_ = float(e.button.y);
            float mx, my; lobbyMouse(mx, my);
            if (e.button.button == SDL_BUTTON_LEFT)
                for (auto& hot : lobbyHots_)
                    if (ptIn(hot.rect, mx, my) && hot.action) { hot.action(); break; }
            return true;
        }
        return false;
    }

    void GameView::buildMapList(tak::LoadScreen* progress) {
        if (progress) progress->step("Finding maps",70);
        resetMinimap(); mapView_.quiesce(); stopPreviewReaders();
        vfs_.refreshMapCache(tak::hpi::userMapRoot());
        mapList_.clear();
        mapListScanned_ = true;
        mapTitlesLoaded_ = false;   // the install may have changed: read the tables again
        auto maps=tak::hpi::listMaps(vfs_);
        // A per-user MapCache copy ("name [16 hex]": a map received from a server, or a
        // Cartographer Test Map snapshot) is left out while an installed map has the same
        // name: those are older or other versions of a map the player has. A copy of a map
        // the player does not have stays. Joining and Test Map pick the copy by its exact
        // name, so they are unaffected.
        {
            const auto lower = [](std::string t) {
                for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
                return t;
            };
            const auto cacheBase = [](const std::string& n) -> std::string {
                if (n.size() > 19 && n.compare(n.size() - 19, 2, " [") == 0 && n.back() == ']' &&
                    std::all_of(n.end() - 17, n.end() - 1, [](char c) { return std::isxdigit((unsigned char)c) != 0; }))
                    return n.substr(0, n.size() - 19);
                return {};
            };
            std::set<std::string> installed;
            for (const auto& [name, path] : maps)
                if (cacheBase(name).empty()) installed.insert(lower(name));
            std::erase_if(maps, [&](const auto& entry) {
                const std::string base = cacheBase(entry.first);
                return !base.empty() && installed.count(lower(base));
            });
        }
        size_t completed=0;
        uint64_t lastProgress=0;
        for (const auto& [name, path] : maps) {
            MapInfo mi; mi.name = name; mi.path = path;
            std::string missionName;
            std::string otaPath = tak::vpath::replaceExtension(path,".ota");
            if (vfs_.has(otaPath)) {
                try {
                    auto b = vfs_.read(otaPath);
                    auto root = tak::tdf::parseText(std::string(b.begin(), b.end()), otaPath);
                    const auto* gh = root.child("globalheader");
                    if (gh) {
                        missionName = gh->valueOr("missionname", "");
                        mi.players = int(gh->numberOr("numplayers", 0));
                        std::string sz = gh->valueOr("size", "");   // e.g. "16 x 16"
                        int a = 0, c = 0;
                        if (std::sscanf(sz.c_str(), "%d x %d", &a, &c) == 2) { mi.sizeW = a; mi.sizeH = c; }
                    }
                    // MAP SCENARIO: only a map that declares a scenario (or a Test Map copy)
                    // can have one, so the CRT of every other map is never read here.
                    if ((gh && gh->numberOr("hasscenario", 0) != 0) || root.child("takplaytest"))
                        mi.scenario = mapScenarioInfo(path);
                } catch (const std::exception&) {}
            }
            // A saved random map's .ota has no size: its .tnt header has (cells, 32 to a unit).
            if (mi.sizeW == 0 && vfs_.has(tak::vpath::replaceExtension(path, ".recipe"))) {
                try {
                    const auto d = vfs_.read(path);
                    auto u32 = [&](size_t o) { return int(uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8)
                                                          | (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24)); };
                    if (d.size() >= 12) { mi.sizeW = u32(4) / 32; mi.sizeH = u32(8) / 32; }
                } catch (const std::exception&) {}
            }
            mi.starts = int(tak::sim::parseStartPositions(vfs_, path).size());
            if (mi.players == 0)   // .ota had no numplayers: count the start positions
                mi.players = mi.starts;
            mi.title = mapTitleFor(name, missionName);
            mapList_.push_back(std::move(mi));
            ++completed;
            if (progress && (completed==maps.size() || SDL_GetTicks64()-lastProgress>=100)) {
                progress->step("Reading maps "+std::to_string(completed)+" / "+std::to_string(maps.size()),
                               70+int(28*completed/maps.size()));
                lastProgress=SDL_GetTicks64();
            }
        }
        sortMapList();
    }

    void GameView::loadMapTitles() {
        mapTitlesLoaded_ = true;
        mapTitles_.clear();
        for (const char* path : {"translate/maps.tdf", "translate/ipmaps.tdf"}) {
            if (!vfs_.has(path)) continue;
            try {
                const auto b = vfs_.read(path);
                const auto root = tak::tdf::parseText(std::string(b.begin(), b.end()), path);
                for (const auto& [key, node] : root.children) {
                    const std::string* en = node.value("english");
                    if (!en || en->empty()) continue;
                    std::string k = key;
                    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                    mapTitles_.emplace(std::move(k), *en);
                }
            } catch (const std::exception&) {}
        }
    }

    // The title of a map file: retail's translated title when the tables name it (only
    // when it is the same words -- a case change, never a different map's title), else
    // the .ota's missionname under the same rule, else the name as the file spells it.
    // A received map's cached copy ("ulasem arena [1e9dfbbdc674b352]") keeps its tag.
    std::string GameView::mapTitleFor(const std::string& name, const std::string& missionName) {
        if (!mapTitlesLoaded_) loadMapTitles();
        auto lower = [](std::string t) { std::transform(t.begin(), t.end(), t.begin(), ::tolower); return t; };
        std::string base = name, tag;
        if (name.size() > 19 && name.compare(name.size() - 19, 2, " [") == 0 && name.back() == ']' &&
            std::all_of(name.end() - 17, name.end() - 1, [](char c) { return std::isxdigit((unsigned char)c) != 0; })) {
            base = name.substr(0, name.size() - 19); tag = name.substr(name.size() - 19);
        }
        const std::string key = lower(base);
        if (auto it = mapTitles_.find(key); it != mapTitles_.end() && lower(it->second) == key) return it->second + tag;
        if (!missionName.empty() && lower(missionName) == key) return missionName + tag;
        return name;
    }

    // What to SHOW for a map id. A generated map's id is a "~gen1~<hex>" recipe -- the
    // whole parameter block in hex -- which is unreadable in a lobby row: it shows the
    // label mapgen provides ("Random 16x16 4P Aramon"). A real map's id is its name: the
    // map list's title for it when the list is built, else the translate tables' title.
    std::string GameView::mapTitle(const std::string& id) {
        if (tak::mapgen::isGeneratedMapId(id)) return tak::mapgen::friendlyLabel(tak::mapgen::decodeMapId(id));
        for (const MapInfo& m : mapList_)
            if (m.name == id) return m.title.empty() ? m.name : m.title;
        return mapTitleFor(id);
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

    void GameView::buildMapPreview(const std::string& tntPath, const tak::hpi::Vfs* src) {
        const bool gen = tak::mapgen::isGeneratedMapId(tntPath);
        ++genSeq_;   // any newer preview supersedes a roll still running
        genPreviewPending_ = false;
        mapPreviewFor_ = tntPath;
        // Another random map keeps the last one's picture until its own is rolled.
        if (!(gen && mapPreviewIsGen_ && mapPreviewTex_)) {
            if (mapPreviewTex_) { gpuvram::destroy(mapPreviewTex_); mapPreviewTex_ = nullptr; }
            mapPreviewW_ = mapPreviewH_ = 0;
            mapPreviewDims_.clear();
            mapPreviewStarts_.clear();
            mapPreviewWorldW_ = mapPreviewWorldH_ = 0;
        }
        mapPreviewIsGen_ = gen;
        if (tntPath.empty()) return;
        genPreviewError_.clear();
        if (gen) {
            // Done with old rolls: drop them (a running one finishes on its own and
            // its result is ignored; its future would block here, so it stays listed --
            // it still reads vfs_, and stopPreviewReaders waits for it like the newest).
            std::erase_if(genJobs_, [](auto& job) {
                return job.second.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            });
            genJobs_.emplace_back(genSeq_, std::async(std::launch::async, [this, id = tntPath] {
                return rollGenPreview(id, vfs_);
            }));
            genPreviewPending_ = true;
            return;
        }
        const tak::hpi::Vfs& from = src ? *src : vfs_;
        std::vector<uint8_t> d;
        try { d = from.read(tntPath); } catch (...) { return; }
        if (d.size() < 52) return;
        auto u32 = [&](size_t o) { return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8)
                 | (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24); };
        // The radar pictures cover the whole map (16 px cells), so a start's world
        // position scales straight onto the drawn image.
        mapPreviewStarts_ = tak::sim::parseStartPositions(from, tntPath);
        mapPreviewWorldW_ = float(u32(4)) * 16; mapPreviewWorldH_ = float(u32(8)) * 16;
        std::string ota;
        if (!src) ota = readOta(tntPath);
        else if (auto o = src->tryRead(tak::vpath::replaceExtension(tntPath, ".ota"))) ota.assign(o->begin(), o->end());
        int w = 0, h = 0; const uint8_t* idx = nullptr;
        std::vector<uint8_t> rgba;
        if (tntIndexedImage(d, 12, w, h, idx) || tntIndexedImage(d, 11, w, h, idx)) {
            const std::vector<uint8_t>* pal = kingdomPalette(otaField(ota, "kingdom"));
            if (!pal) pal = kingdomPalette("aramon");   // maps without a kingdom get a default
            if (!pal) return;
            rgba.resize(size_t(w) * h * 4);
            for (size_t i = 0; i < size_t(w) * h; ++i) {
                uint8_t p = idx[i];
                if (p == 9) { rgba[i * 4 + 3] = 0; continue; }   // retail's transparent index
                const uint8_t* c = &(*pal)[size_t(p) * 4];
                rgba[i * 4 + 0] = c[0]; rgba[i * 4 + 1] = c[1]; rgba[i * 4 + 2] = c[2]; rgba[i * 4 + 3] = 255;
            }
        } else {
            // No radar picture (a random map saved before they were written): build it
            // from the tiles, as the save now does.
            std::string kingdom = otaField(ota, "kingdom");
            for (auto& ch : kingdom) ch = char(std::tolower(uint8_t(ch)));
            if (kingdom.empty()) kingdom = "aramon";
            try { auto m = tak::tnt::Map::load(d, tntPath); rgba = radarPicture(m, from, kingdom, w, h); }
            catch (const std::exception&) { return; }
            if (rgba.empty()) return;
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
        std::string size = otaField(ota, "size");
        if (size.empty()) size = std::to_string(u32(4) / 32) + " x " + std::to_string(u32(8) / 32);
        std::string players = otaField(ota, "numplayers");
        if (players.empty() && !mapPreviewStarts_.empty())
            players = std::to_string(mapPreviewStarts_.size());
        mapPreviewDims_ = size + (players.empty() ? "" : "   " + players + " PLAYER");
    }

    std::vector<uint8_t> GameView::radarPicture(tak::tnt::Map& m, const tak::hpi::Vfs& vfs, const std::string& world,
                                                int& w, int& h) {
        // The large radar picture a saved map carries, built from the map's tiles as the
        // Cartographer builds it, through the world's palette (index 9 transparent, as
        // buildMapPreview draws a shipped map's).
        if (m.overview.empty() || m.overviewW <= 0 || m.overviewH <= 0) {
            tak::terrain::Compositor compositor(vfs);
            tak::terrain::generateMinimaps(m, compositor, tak::terrain::loadWorldPalette(vfs, world));
        }
        w = m.overviewW; h = m.overviewH;
        if (w <= 0 || h <= 0 || m.overview.size() < size_t(w) * h) return {};
        const auto pal = tak::terrain::loadWorldPalette(vfs, world);
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            const uint8_t p = m.overview[i];
            if (p == 9) { rgba[i * 4 + 3] = 0; continue; }
            std::copy(pal.rgba[p], pal.rgba[p] + 4, rgba.begin() + i * 4);
        }
        return rgba;
    }

    GameView::GenPreviewOut GameView::rollGenPreview(const std::string& id, const tak::hpi::Vfs& vfs) {
        // On a worker: roll the map from the seed and build its radar picture from the
        // tiles, as a saved map carries it; pollGenPreview uploads it. Nothing here touches the view.
        GenPreviewOut out;
        out.id = id;
        tak::mapgen::Params gp;
        tak::mapgen::Result g;
        try { gp = tak::mapgen::parseMapId(id); g = tak::mapgen::generate(gp, vfs); }
        catch (const std::exception& e) { out.error = e.what(); return out; }
        const int W = g.map.width, H = g.map.height;
        int TW = 0, TH = 0;
        auto rgba = radarPicture(g.map, vfs, tak::mapgen::worldKey(gp.mapType), TW, TH);
        out.mountains = g.mountains;
        if (rgba.empty()) return out;
        // Start positions: drawn over the thumbnail as numbered markers (drawStartMarkers),
        // in world px like a shipped map's -- the same cell -> px setupMatch applies.
        for (auto& [sx, sz] : g.starts) out.starts.push_back({float(sx * 16), float(sz * 16)});
        out.worldW = float(W) * 16; out.worldH = float(H) * 16;
        out.rgba = std::move(rgba); out.w = TW; out.h = TH;
        char dims[48];
        std::snprintf(dims, sizeof dims, "%d x %d   %d PLAYER", W / 32, H / 32, int(gp.players));
        out.dims = dims;
        out.map = std::make_shared<const tak::tnt::Map>(std::move(g.map));
        return out;
    }

    void GameView::stopPreviewReaders() {
        bigMap_.reset();   // ~MapView joins its section and chunk workers
        for (auto& job : genJobs_) job.second.wait();
    }

    void GameView::pollGenPreview() {
        for (auto it = genJobs_.begin(); it != genJobs_.end();) {
            if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
            GenPreviewOut out = it->second.get();
            const bool newest = it->first == genSeq_;
            it = genJobs_.erase(it);
            if (!newest) continue;
            genPreviewPending_ = false;
            if (mapPreviewTex_) { gpuvram::destroy(mapPreviewTex_); mapPreviewTex_ = nullptr; }
            mapPreviewW_ = mapPreviewH_ = 0;
            mapPreviewStarts_ = std::move(out.starts);
            mapPreviewWorldW_ = out.worldW; mapPreviewWorldH_ = out.worldH;
            mapPreviewDims_ = std::move(out.dims);
            genPreviewError_ = std::move(out.error);
            if (out.map) { genPreviewMap_ = std::move(out.map); genPreviewMapId_ = out.id; genPreviewMountains_ = out.mountains; }
            if (out.w <= 0) continue;
            mapPreviewTex_ = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, out.w, out.h);
            if (!mapPreviewTex_) continue;
            SDL_SetTextureBlendMode(mapPreviewTex_, SDL_BLENDMODE_BLEND);
            SDL_UpdateTexture(mapPreviewTex_, nullptr, out.rgba.data(), out.w * 4);
            SDL_SetTextureScaleMode(mapPreviewTex_, SDL_ScaleModeLinear);
            mapPreviewW_ = out.w; mapPreviewH_ = out.h;
        }
    }

    void GameView::applyGenParams() {
        genParams_ = tak::mapgen::sanitize(genParams_);   // grows a side the layout cannot fit
        // The generator's target: the CHANGE MAP panel's RANDOM candidate.
        std::string& id = picker_.gen.id;
        std::string& path = picker_.gen.path;
        auto previous = tak::mapgen::decodeMapId(id);
        previous.name = genParams_.name;
        const bool reusePreview = mapPreviewFor_ == path &&
            tak::mapgen::isGeneratedMapId(id) &&
            tak::mapgen::encodeMapId(previous) == tak::mapgen::encodeMapId(genParams_);
        const std::string next = tak::mapgen::encodeMapId(genParams_);
        if (next != id) picker_.message.clear();   // a new candidate: the last PICK's answer is old news
        id = next;
        if (reusePreview) mapPreviewFor_ = id;
        path = id;   // generated: the id IS the path (findMap returns it as-is)
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
            case 4: genParams_.reliefDensity = v; break;
            case 6: genParams_.plateauDensity = v; break;
            case 7: genParams_.mountainDensity = v; break;
            case 8: genParams_.townDensity = v; break;
            default:   // SCATTERED MANA snaps to its five levels
                genParams_.manaScatter = uint8_t(t * float(tak::mapgen::kScatterLevels - 1) + 0.5f); break;
        }
    }

    void GameView::scanModPacks() {
        if(!modPacksScanned_){
            const auto root=tak::u8path(installRoot_);
            availableGameMods_=mods_.game?tak::hpi::modPacks(root,tak::hpi::ModKind::Game):std::vector<std::string>{};
            availableCosmeticMods_=mods_.cosmetic?tak::hpi::modPacks(root,tak::hpi::ModKind::Cosmetic):std::vector<std::string>{};
            alwaysCosmeticMods_=mods_.cosmetic?tak::hpi::alwaysEnabledPacks(root,tak::hpi::ModKind::Cosmetic):std::vector<std::string>{};
            alwaysGameMods_=mods_.game?tak::hpi::alwaysEnabledPacks(root,tak::hpi::ModKind::Game):std::vector<std::string>{};
            modPacksScanned_=true;
        }
    }

    // The mods screen (the room's MODS... button): COSMETIC and GAME side by side over the
    // whole panel, each with its FILTER and a scrolling list of tick boxes, the
    // always-enabled packs first, locked on. COSMETIC packs are this player's own (never
    // sent, never changing the game); GAME packs only the skirmish host ticks, and a guest
    // sees the host's choice. At most 64 packs of a kind, its always-enabled ones included.
    // Changes apply as they are ticked; DONE (or Esc) goes back to the room.
    void GameView::drawModsScreen(int winW, int winH) {
        (void)winH;
        const auto& room = mp_->room();
        const bool host = room.hostId == mp_->myClientId();
        scanModPacks();
        const bool gameOn = room.mission.empty() && room.opts.gameMods;
        const auto& alwaysGame = mp_->alwaysGameModPacks();
        // (The page title, MODS, is the lobby's own.)
        constexpr float kTop = 60, kRowH = 22, kListTop = 112, kListBottom = 482, kPx = 1.5f;
        const float colW = (winW - 2 * kCreateMargin - 16) / 2;
        // One column: heading, count, FILTER, then the list box.
        auto column = [&](int k, const char* heading, const std::vector<std::string>& available,
                          const std::vector<std::string>& selected, const std::vector<std::string>& always,
                          std::string& filter, int field, bool editable, const char* note,
                          std::function<void(std::vector<std::string>)> apply) {
            const float cx = kCreateMargin + float(k) * (colW + 16);
            blockText(heading, cx, kTop, 1.8f, {205, 210, 225, 255});
            const auto key = [](const std::string& n) { return tak::hpi::MountSet::key(n); };
            const auto isAlways = [&](const std::string& n) {
                return std::any_of(always.begin(), always.end(), [&](const std::string& a) { return key(a) == key(n); });
            };
            std::vector<std::string> names = available;
            for (const auto& n : selected)
                if (std::find(names.begin(), names.end(), n) == names.end() && !isAlways(n)) names.push_back(n);
            std::sort(names.begin(), names.end());
            size_t enabled = always.size();
            for (const auto& n : selected) if (!isAlways(n)) ++enabled;
            const std::string count = std::to_string(enabled) + " / 64 ON";
            blockText(count, cx + colW - blockWidth(count, 1.4f), kTop + 4, 1.4f, {160, 170, 185, 255});
            // FILTER.
            const SDL_FRect fr{cx, kTop + 24, colW, kRowH};
            const bool active = lbField_ == field;
            SDL_SetRenderDrawColor(ren_, 24, 26, 34, 255); SDL_RenderFillRectF(ren_, &fr);
            SDL_SetRenderDrawColor(ren_, active ? 200 : 90, active ? 210 : 100, active ? 130 : 130, 255); SDL_RenderDrawRectF(ren_, &fr);
            if (filter.empty() && !active) blockText("FILTER", fr.x + 6, fr.y + 5, kPx, {110, 116, 132, 255});
            else blockText(tak::uiTextFitStart(filter + (active ? "_" : ""), kPx, fr.w - 12), fr.x + 6, fr.y + 5, kPx, {225, 230, 240, 255});
            lobbyHots_.push_back({fr, [this, field] { lbField_ = field; SDL_StartTextInput(); }});
            const std::string want = tak::mapfilter::lower(filter);
            // The rows: the always-enabled packs (locked), then the tickable ones.
            struct Row { std::string name; bool locked; };
            std::vector<Row> rows;
            for (const auto& a : always) if (tak::mapfilter::matches(a, "", want)) rows.push_back({a, true});
            for (const auto& n : names) if (tak::mapfilter::matches(n, "", want)) rows.push_back({n, false});
            const SDL_FRect box{cx, kListTop, colW, kListBottom - kListTop};
            modListRect_[k] = box;
            SDL_SetRenderDrawColor(ren_, 24, 26, 34, 255); SDL_RenderFillRectF(ren_, &box);
            SDL_SetRenderDrawColor(ren_, 70, 76, 96, 255); SDL_RenderDrawRectF(ren_, &box);
            if (note) { blockText(note, cx + 8, kListTop + 8, 1.4f, {150, 155, 165, 255}); return; }
            if (rows.empty()) {
                blockText(names.empty() && always.empty() ? (k ? "NO PACKS IN MODS/GAME" : "NO PACKS IN MODS/COSMETIC")
                                                          : "NO PACK MATCHES THE FILTER",
                          cx + 8, kListTop + 8, 1.4f, {150, 155, 165, 255});
                return;
            }
            const int visible = int((box.h - 4) / kRowH);
            modScroll_[k] = std::clamp(modScroll_[k], 0, std::max(0, int(rows.size()) - visible));
            const float sbW = int(rows.size()) > visible ? 10.0f : 0.0f;
            for (int r = 0; r < visible && modScroll_[k] + r < int(rows.size()); ++r) {
                const Row& row = rows[size_t(modScroll_[k] + r)];
                const SDL_FRect rr{cx + 2, kListTop + 2 + float(r) * kRowH, colW - 4 - sbW, kRowH - 2};
                const bool on = row.locked || std::find(selected.begin(), selected.end(), row.name) != selected.end();
                const bool live = editable && !row.locked;
                if (live) {
                    const std::string name = row.name;
                    lbBtn(rr.x, rr.y, rr.w, rr.h, "", true, [selected, name, on, apply, n = always.size()] {
                        auto selection = selected;
                        if (on) selection.erase(std::remove(selection.begin(), selection.end(), name), selection.end());
                        else if (selection.size() + n + 1 <= 64) selection.push_back(name);
                        apply(std::move(selection));
                    }, {40, 44, 58, 255}, kPx);
                }
                SDL_FRect check{rr.x + 8, rr.y + 5, 11, 11};
                SDL_SetRenderDrawColor(ren_, live ? 190 : 120, live ? 205 : 128, live ? 220 : 140, 255);
                SDL_RenderDrawRectF(ren_, &check);
                if (on) { check.x += 3; check.y += 3; check.w -= 6; check.h -= 6; SDL_RenderFillRectF(ren_, &check); }
                blockText(tak::uiTextFit(row.name, kPx, rr.w - 120), rr.x + 28, rr.y + 4, kPx,
                          live ? SDL_Color{225, 230, 240, 255} : SDL_Color{170, 200, 170, 255});
                if (row.locked) {
                    const char* tag = "ALWAYS ON";
                    blockText(tag, rr.x + rr.w - blockWidth(tag, 1.2f) - 8, rr.y + 5, 1.2f, {150, 185, 150, 255});
                }
            }
            if (sbW > 0) {   // where the visible rows sit in the list
                const float th = std::max(20.0f, box.h * float(visible) / float(rows.size()));
                const float ty = box.y + (box.h - th) * float(modScroll_[k]) / float(int(rows.size()) - visible);
                SDL_FRect thumb{box.x + box.w - sbW - 1, ty, sbW, th};
                SDL_SetRenderDrawColor(ren_, 90, 100, 130, 255); SDL_RenderFillRectF(ren_, &thumb);
            }
        };
        column(0, "COSMETIC MODS", availableCosmeticMods_, settings_ ? settings_->cosmeticModPacks : std::vector<std::string>{},
               alwaysCosmeticMods_, cosmeticModFilter_, 7, room.mission.empty(), nullptr,
               [this](std::vector<std::string> selection) {
                   if (!settings_) return;
                   settings_->cosmeticModPacks = std::move(selection); saveSettings(*settings_);
               });
        std::string guestNote;
        if (!gameOn) guestNote = "THE HOST TURNED GAME MODS OFF";
        else if (!host) {
            std::string names = mp_->modPackage() ? mp_->modPackage()->mapPath : "";
            guestNote = "THE HOST CHOOSES: " + (names.empty() ? std::string(mp_->modPackage() ? "NONE" : "WAITING FOR THE HOST") : names);
            guestNote = tak::uiTextFit(guestNote, 1.4f, colW - 16);
        }
        column(1, "GAME MODS", availableGameMods_, mp_->gameModPacks(), host ? alwaysGame : std::vector<std::string>{},
               gameModFilter_, 8, host && gameOn && !mp_->modBusy(), guestNote.empty() ? nullptr : guestNote.c_str(),
               [this](std::vector<std::string> selection) {
                   mp_->setGameModPacks(selection);
                   // This room kind's remembered ticks, when this room remembers (not a
                   // --play-map run or a harness); a saved tick hidden by a same-named
                   // always-enabled pack is kept there (rememberRoomPrefs).
                   rememberRoomPrefs(RoomField::Mods);
               });
        if (const auto status = mp_->roomModStatus(); gameOn && !status.empty())
            blockText(tak::uiTextFit(status, 1.3f, 600), kCreateMargin, kLobbyH - 32, 1.3f, {205, 190, 135, 255});
        lbBtn(kLobbyW - kCreateMargin - 120, kLobbyH - 40, 120, 30, "DONE", true, [this] { modsScreen_ = false; lbField_ = 0; SDL_StopTextInput(); });
    }

    void GameView::drawRoomColumn(const RoomFrame& f, int winH) {
        const auto& room = mp_->room();
        const float colX = f.colX, colW = f.colW;
        const bool host = f.host, roomMap = f.roomMap;
        // The map's name alone (the preview under it says what it names), left-aligned with
        // that preview and kept to the column: a long name (a Test Map copy's
        // "test <n> [<fingerprint>]") shrinks a little, then is cut with "...".
        // A map or MAP SCENARIO change lights it, and the preview's rim below.
        const SDL_FRect head{colX, 58, colW, 20};
        drawRoomFlash(RoomField::Map, head); drawRoomFlash(RoomField::Scenario, head);
        const std::string mapHead = mapTitle(room.mapId);
        const float mapHeadPx = tak::uiTextFitPx(mapHead, 1.8f, colW, 1.4f);
        blockText(tak::uiTextFit(mapHead, mapHeadPx, colW), colX, 62 + (1.8f - mapHeadPx) * 4, mapHeadPx, {180, 185, 195, 255});
        // The room's map, with its starts numbered and coloured by who opens there,
        // heads the right column; the mod packs and the chat sit right under it.
        // The preview is as wide as the column and as tall as the map's shape makes it.
        // Single player has no chat, so a square map takes the column's whole width; a
        // multiplayer room keeps room under the mods line for a few rows of chat, and under
        // the preview for the host's CHANGE MAP button -- for every player, so host and
        // guests see the same preview (a guest's chat gets the button's room).
        constexpr float kPreviewY = 84, kChatInputH = 52, kChatMinH = 104, kModsH = 44, kChangeMapH = 26;
        const float previewMaxH = singlePlayer_ ? colW
                                : float(winH) - kChatInputH - 6 - kChatMinH - kModsH - kPreviewY - kChangeMapH;
        // While the CHANGE MAP panel is open its candidate owns the shared preview texture:
        // the room keeps the size its own preview had (roomPreviewSize_).
        if (roomMap && !picker_.open) { syncRoomPreview(); roomPreviewSize_ = {mapPreviewW_, mapPreviewH_}; }
        const SDL_FRect pv = roomMap ? roomPreviewBox(colX, kPreviewY, previewMaxH) : SDL_FRect{colX, kPreviewY, 0, 0};
        // CHANGE MAP... (the host of a skirmish room; not a --play-map room, which exists to
        // test its one map): the panel over the room (gameview_mappicker.cpp). Shown while a
        // START or a map change is pending too, disabled, so the lines under it stay put.
        const bool changeMap = roomMap && host && !room.opts.benchmark && roomForcedMap_.empty();
        if (changeMap)
            lbBtn(colX, pv.y + pv.h + 6, 120, 20, "CHANGE MAP...", mp_->roomEditable() && !mp_->roomMapPending(),
                  [this] { openMapPicker(); }, {60, 66, 86, 255}, 1.3f);
        float modBottom=0;
        {
            // MODS, one line under the map: what loads, as many names as fit then +N,
            // each in its kind's colour -- green the always-enabled packs
            // (mods/<kind>/always_enabled/<pack>, locked, never ticked; the cosmetic ones
            // load in every game, campaigns included), grey the cosmetic packs this player
            // ticked (not in a mission room; never sent anywhere, cannot change the game),
            // blue the game packs. Only the host of a skirmish room ticks mods/game packs;
            // they and the host's always-enabled game packs are sent to every player, and
            // everyone else sees the host's choice. The MODS button at the line's end opens
            // the mods screen (drawModsScreen), where every list is shown in full; the game
            // mods' status (transfer, error, VERIFIED) is a small line under it. Game mods
            // are OFF only for a debug --mods launch; campaign mission and Benchmark rooms
            // are always off.
            scanModPacks();
            const bool gameRoom=room.mission.empty();
            const bool gameOn=gameRoom && room.opts.gameMods;
            const auto& alwaysGame=mp_->alwaysGameModPacks();
            const auto splitNames=[](const std::string& joined) {
                std::vector<std::string> out;
                size_t at=0;
                while(at<joined.size()){
                    size_t comma=joined.find(',',at);
                    std::string n=joined.substr(at,comma==std::string::npos?std::string::npos:comma-at);
                    while(!n.empty() && n.front()==' ')n.erase(n.begin());
                    if(!n.empty())out.push_back(n);
                    if(comma==std::string::npos)break;
                    at=comma+1;
                }
                return out;
            };
            constexpr SDL_Color kAlways{170,200,170,255},kCosmetic{200,205,215,255},kGame{150,190,235,255},kDim{150,155,165,255};
            std::vector<std::pair<std::string,SDL_Color>> names;
            for(const auto& n:alwaysCosmeticMods_)names.emplace_back(n,kAlways);
            if(gameOn && host)for(const auto& n:alwaysGame)names.emplace_back(n,kAlways);
            std::vector<std::string> notes;   // the lines under: what the game mods wait for, their status
            if(gameRoom) {
                if(settings_)for(const auto& n:settings_->cosmeticModPacks)
                    if(std::find(availableCosmeticMods_.begin(),availableCosmeticMods_.end(),n)!=availableCosmeticMods_.end())
                        names.emplace_back(n,kCosmetic);
                // Game mods off is not a pack name: it leads the line, so no +N hides it.
                if(!gameOn)names.insert(names.begin(),{"GAME OFF (HOST)",kDim});
                else if(host)for(const auto& n:mp_->gameModPacks())names.emplace_back(n,kGame);
                else if(!mp_->modPackage())notes.push_back("GAME MODS: WAITING FOR THE HOST");
                else for(const auto& n:splitNames(mp_->modPackage()->mapPath))names.emplace_back(n,kGame);
                if(const auto status=mp_->roomModStatus();gameOn && !status.empty())notes.push_back(status);
            }
            constexpr float kPx=1.3f,kBtnW=64;
            const float py=roomMap ? pv.y+pv.h+6+(changeMap ? kChangeMapH : 0) : 87;
            drawRoomFlash(RoomField::Mods,{colX,py-4,colW,18});   // the host changed the game mods
            const float right=colX+colW-(gameRoom ? kBtnW+6 : 0);
            const char* label="MODS: ";
            blockText(label,colX+4,py,kPx,{205,210,225,255});
            float nx=colX+4+blockWidth(label,kPx);
            if(names.empty())blockText("NONE",nx,py,kPx,kDim);
            // Each name keeps room after it for a " +N", in case the next one does not fit.
            for(size_t i=0;i<names.size();++i){
                const bool last=i+1==names.size();
                const std::string sep=i ? ", " : "";
                if(i && nx+blockWidth(sep+names[i].first+(last?"":" +99"),kPx)>right){
                    blockText(" +"+std::to_string(names.size()-i),nx,py,kPx,kDim);
                    break;
                }
                if(i){blockText(sep,nx,py,kPx,kDim);nx+=blockWidth(sep,kPx);}
                // A first name too long for the line is cut.
                const float room=right-nx-(last?0:blockWidth(" +99",kPx));
                const std::string shown=tak::uiTextFit(names[i].first,kPx,std::max(20.0f,room));
                blockText(shown,nx,py,kPx,names[i].second);
                nx+=blockWidth(shown,kPx);
            }
            if(gameRoom)lbBtn(colX+colW-kBtnW,py-4,kBtnW,18,"MODS...",true,[this]{modsScreen_=true;lbField_=0;SDL_StopTextInput();},
                              {60,66,86,255},kPx);
            float below=py+20;
            for(const auto& note:notes){blockText(tak::uiTextFit(note,1.1f,colW-4),colX+4,below-2,1.1f,{205,190,135,255});below+=12;}
            modBottom=below+2;
        }
        // A scenario that places its own armies opens nobody at the map's starts. The
        // flash is drawn first: the preview covers all of it but the rim.
        if (roomMap) {
            const SDL_FRect rim{pv.x - 3, pv.y - 3, pv.w + 6, pv.h + 6};
            drawRoomFlash(RoomField::Map, rim); drawRoomFlash(RoomField::Scenario, rim);
            if (!picker_.open) drawRoomPreview(pv, f.scenarioOn && scenarioLobby_.placed > 0);
            else {   // under the panel: the preview's empty frame
                SDL_SetRenderDrawColor(ren_, 18, 20, 28, 255); SDL_RenderFillRectF(ren_, &pv);
                SDL_SetRenderDrawColor(ren_, 70, 76, 96, 255); SDL_RenderDrawRectF(ren_, &pv);
            }
        }
        // chat panel on the right (multiplayer only -- there's no one to chat with in SP)
        if (!singlePlayer_) {
            // The message box runs from under the mods down to the line the text field and
            // SEND share.
            float chx = colX, chy = modBottom, chw = colW;
            constexpr float kLineH = 16, kSb = 8;
            const float inputY = winH - kChatInputH;
            SDL_SetRenderDrawColor(ren_, 22, 24, 32, 255);
            SDL_FRect cp{chx, chy, chw, std::max(40.0f, inputY - 6 - chy)}; SDL_RenderFillRectF(ren_, &cp);
            chatRect_ = cp;
            // Every message wrapped to the box (measured, so it fits in either UI FONT),
            // oldest first; chatScroll_ is how many rows up from the newest the view sits
            // (the mouse wheel over the box), with a scroll bar once they overflow.
            std::vector<std::string> rows;
            for (const auto& m : chatLog_)
                for (auto& r : tak::uiTextWrap(m.first + ": " + m.second, 1.4f, chw - 16 - kSb)) rows.push_back(std::move(r));
            const int visible = std::max(1, int((cp.h - 8) / kLineH));
            const int total = int(rows.size());
            chatScroll_ = std::clamp(chatScroll_, 0, std::max(0, total - visible));
            const int last = total - 1 - chatScroll_;   // the bottom row on show
            float ly = cp.y + 4 + float(visible - 1) * kLineH;
            for (int i = last; i >= 0 && i > last - visible; --i, ly -= kLineH)
                blockText(rows[size_t(i)], chx + 8, ly, 1.4f, {200, 205, 215, 255});
            if (total > visible) {
                const float th = std::max(16.0f, cp.h * float(visible) / float(total));
                const float ty = cp.y + (cp.h - th) * float(total - visible - chatScroll_) / float(total - visible);
                SDL_FRect thumb{cp.x + cp.w - kSb - 2, ty, kSb, th};
                SDL_SetRenderDrawColor(ren_, 90, 100, 130, 255); SDL_RenderFillRectF(ren_, &thumb);
            }
            // The text field (no label): Enter or SEND sends.
            {
                const SDL_FRect r{chx, inputY, chw - 70, 24};
                const bool active = lbField_ == 4;
                SDL_SetRenderDrawColor(ren_, 24, 26, 34, 255); SDL_RenderFillRectF(ren_, &r);
                SDL_SetRenderDrawColor(ren_, active ? 200 : 90, active ? 210 : 100, active ? 130 : 130, 255); SDL_RenderDrawRectF(ren_, &r);
                const float vpx = tak::modernUiText() ? 1.6f : 1.8f;
                const std::string shown = tak::uiTextFitStart(chatDraft_ + (active ? "_" : ""), vpx, r.w - 12);
                blockText(shown, r.x + 6, r.y + (r.h - 7 * vpx - (tak::modernUiText() ? tak::uiTextDescent(vpx) : 0)) / 2, vpx,
                          {225, 230, 240, 255});
                lobbyHots_.push_back({r, [this] { lbField_ = 4; SDL_StartTextInput(); }});
            }
            lbBtn(chx + chw - 62, inputY, 56, 24, "SEND", !chatDraft_.empty(), [this] {
                mp_->chat(chatDraft_); chatDraft_.clear(); chatScroll_ = 0; });
        }
        // A generated map's recipe, under the pointer on its preview: a box as wide as the
        // column (a tall map's preview is narrow) in the lower half of the preview, clear
        // of the CLICK band, or hung under it (over the mods and the chat) when the preview
        // is too short to hold it -- never past the bottom of the screen.
        if (roomMap && !picker_.open && lbHot(pv) && !genPreviewPending_ && mapPreviewTex_) {
            const auto lines = packItems(genRecipeItems(room.mapId, genPreviewMountainsFor(room.mapId)), 1.1f, colW - 12);
            if (!lines.empty()) {
                constexpr float kPitch = 13;
                const float h = float(lines.size()) * kPitch + 8;
                const bool inside = pv.h / 2 + 14 + h <= pv.h;
                const float by = inside ? pv.y + pv.h - h : std::max(pv.y, std::min(pv.y + pv.h + 1, float(winH) - 4 - h));
                const SDL_FRect bx{colX, by, colW, h};
                SDL_SetRenderDrawColor(ren_, 12, 14, 20, inside ? 235 : 255); SDL_RenderFillRectF(ren_, &bx);
                SDL_SetRenderDrawColor(ren_, 70, 76, 96, 255); SDL_RenderDrawRectF(ren_, &bx);
                for (size_t k = 0; k < lines.size(); ++k)
                    blockText(lines[k], bx.x + 6, bx.y + 5 + float(k) * kPitch, 1.1f, {205, 210, 225, 255});
            }
        }
    }

    std::vector<std::vector<std::string>> GameView::genRecipeItems(const std::string& mapId, int mountains) {
        if (!tak::mapgen::isGeneratedMapId(mapId)) return {};
        const auto gp = tak::mapgen::decodeMapId(mapId);
        const auto level = [](uint8_t v) { return v == 0 ? "NONE" : v < 86 ? "LOW" : v < 171 ? "MED" : "HIGH"; };
        const auto kit = [&](uint8_t v, uint8_t which) { return tak::mapgen::kitAvailable(gp.mapType, which) ? level(v) : "NONE"; };
        // Asked for, but the roll found no room (small or crowded maps): say so, not the amount.
        const bool noRoom = gp.mountainDensity && tak::mapgen::kitAvailable(gp.mapType, tak::mapgen::KitMountains) &&
                            mountains == 0;
        std::string scatter = tak::mapgen::scatterName(gp.manaScatter);
        for (char& ch : scatter) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        return {{std::string("MAP TYPE: ") + tak::mapgen::worldTitle(gp.mapType),
                std::string("LAYOUT: ") + tak::mapgen::layoutName(gp.layout),
                "SIZE: " + std::to_string(gp.widthCells / 32) + " X " + std::to_string(gp.heightCells / 32),
                "PLAYERS: " + std::to_string(gp.players),
                std::string("FORESTS: ") + (tak::mapgen::worldHasForests(gp.mapType) ? level(gp.treeDensity) : "NONE"),
                std::string(tak::mapgen::lavaWorld(gp.mapType) ? "RUINS: " : "ROCKS: ") + level(gp.rockDensity),
                std::string("MANA: ") + level(gp.manaDensity),
                gp.layout == tak::mapgen::IndustrialZone ? std::string("LAVA PADS: ") + level(gp.waterDensity) :
                std::string(tak::mapgen::lavaWorld(gp.mapType) ? "LAVA: " : "WATER: ") +
                    (tak::mapgen::automaticWater(gp.layout) ? "AUTO" : level(gp.waterDensity)),
                std::string("HILLS: ") + (tak::mapgen::hillsPossible(gp.mapType, gp.layout) ? level(gp.reliefDensity) :
                                       gp.layout == tak::mapgen::Maze ? "AUTO" : "NONE")},
               {std::string("PLATEAUS: ") + (tak::mapgen::automaticPlateaus(gp.layout) ? "AUTO" : kit(gp.plateauDensity, tak::mapgen::KitPlateaus)),
                std::string("MOUNTAINS: ") + (noRoom ? "NO ROOM" : kit(gp.mountainDensity, tak::mapgen::KitMountains)),
                std::string(gp.mapType == tak::mapgen::Zhon ? "VILLAGES: " : "TOWNS: ") + kit(gp.townDensity, tak::mapgen::KitTowns),
                "SCATTERED MANA: " + scatter,
                "MAP SEED: " + std::to_string(gp.seed)}};
    }

    std::vector<std::string> GameView::packItems(const std::vector<std::vector<std::string>>& groups, float px, float maxW) const {
        std::vector<std::string> lines;
        for (const auto& items : groups) {
            bool fresh = true;   // each group starts a line
            for (const auto& item : items) {
                if (!fresh && blockWidth(lines.back() + "  " + item, px) <= maxW) lines.back() += "  " + item;
                else lines.push_back(tak::uiTextFit(item, px, maxW));
                fresh = false;
            }
        }
        return lines;
    }

    GameView::RoomStarts GameView::roomStarts() {
        const auto& room = mp_->room();
        // A generated map has a start for each player its recipe (the map id) names.
        if (tak::mapgen::isGeneratedMapId(room.mapId)) {
            try { return {int(tak::mapgen::decodeMapId(room.mapId).players), false}; }
            catch (const std::exception&) { return {}; }   // invalid ids cannot start
        }
        // A file map: read its verified package (the host's own until then) once.
        const auto& package = mp_->mapPackage();
        if (!package || !package->files) return {};
        if (roomStartsPackage_.lock() != package) {
            roomStartsPackage_ = package;
            roomStarts_ = {};
            try {
                tak::hpi::Vfs view; view.setMapFiles(package->files);
                roomStarts_.count = int(tak::sim::parseStartPositions(view, package->mapPath).size());
                if (tak::net::maps::authoredScenario(*package))
                    if (const auto crt = view.tryRead(tak::vpath::replaceExtension(package->mapPath, ".crt")))
                        roomStarts_.placed = !tak::crt::parse(*crt).units.empty();
            } catch (const std::exception&) {}
        }
        return roomStarts_;
    }

    void GameView::drawStartMarkers(const SDL_FRect& img, const std::vector<int>& colorOfStart, float maxRadius) {
        if (mapPreviewStarts_.empty() || img.w <= 0 || img.h <= 0) return;
        // Sized from the preview (the full-window map view allows larger), so the CHANGE MAP
        // panel's previews (220 and 336 px) and the room's both read: a black rim and a white
        // ring keep it legible on any terrain.
        const float r = std::clamp(std::min(img.w, img.h) * 0.06f, 7.0f, maxRadius);
        auto disc = [&](float cx, float cy, float rad, SDL_Color c) {
            constexpr int kSeg = 20;
            SDL_Vertex v[kSeg + 1];
            int ix[kSeg * 3];
            v[0] = {{cx, cy}, c, {0, 0}};
            for (int k = 0; k < kSeg; ++k) {
                const float a = float(k) * 6.2831853f / kSeg;
                v[k + 1] = {{cx + rad * std::cos(a), cy + rad * std::sin(a)}, c, {0, 0}};
                ix[k * 3] = 0; ix[k * 3 + 1] = k + 1; ix[k * 3 + 2] = (k + 1) % kSeg + 1;
            }
            SDL_RenderGeometry(ren_, nullptr, v, kSeg + 1, ix, kSeg * 3);
        };
        for (size_t k = 0; k < mapPreviewStarts_.size(); ++k) {
            const auto p = tak::startmarkers::place(mapPreviewStarts_[k], mapPreviewWorldW_, mapPreviewWorldH_,
                                                    img.x, img.y, img.w, img.h, r + 2);
            const int color = k < colorOfStart.size() ? colorOfStart[k] : -1;
            // Neutral (nobody, or nobody knows yet) is a dark disc: the slot palette has
            // both a white and a grey, so neither can stand for "no one".
            const SDL_Color fill = color >= 0 ? playerColors_[color % 10] : SDL_Color{34, 36, 46, 255};
            disc(p.x, p.y, r + 2, {0, 0, 0, 255});
            disc(p.x, p.y, r + 1, {235, 235, 240, 255});
            disc(p.x, p.y, r, fill);
            const std::string n = std::to_string(k + 1);
            const float px = r * (n.size() > 1 ? 0.15f : 0.19f);
            const bool light = fill.r * 299 + fill.g * 587 + fill.b * 114 > 150000;
            blockText(n, p.x - blockWidth(n, px) / 2 + px / 2, p.y - 3.5f * px, px,
                      light ? SDL_Color{16, 16, 20, 255} : SDL_Color{250, 250, 252, 255});
        }
    }

    void GameView::syncRoomPreview() {
        const auto& room = mpRoom();
        // A verified map package is the map every peer plays (a joiner may not have it,
        // or may hold a different map under the same name); otherwise the room's map is
        // in the VFS, or it is a generated id. Rebuilt only when the map changes.
        const auto pkg = mp_->mapPackage();
        const bool gen = tak::mapgen::isGeneratedMapId(room.mapId);
        const std::string key = "room:" + room.mapId + "#" + (pkg && !gen ? pkg->digest : std::string());
        if (mapPreviewFor_ != key) {
            if (pkg && !gen && pkg->files) {
                tak::hpi::Vfs view; view.setMapFiles(pkg->files);
                buildMapPreview(pkg->mapPath, &view);
            } else {
                buildMapPreview(gen ? room.mapId : tak::hpi::findMap(vfs_, room.mapId));
            }
            mapPreviewFor_ = key;
        }
    }

    SDL_FRect GameView::roomPreviewBox(float x, float y, float maxH) const {
        // The map's shape: a generated map's from its id (known while it is still being
        // rolled, when the last map's picture is on show), a file map's from its picture;
        // square while neither is known.
        const auto& id = mpRoom().mapId;
        float w = 1, h = 1;
        if (tak::mapgen::isGeneratedMapId(id)) {
            const auto gp = tak::mapgen::decodeMapId(id);
            w = gp.widthCells; h = gp.heightCells;
        } else if (picker_.open) {   // the CHANGE MAP panel's candidate owns the picture: the room's size, kept
            if (roomPreviewSize_.x > 0 && roomPreviewSize_.y > 0) { w = float(roomPreviewSize_.x); h = float(roomPreviewSize_.y); }
        } else if (mapPreviewTex_ && mapPreviewW_ > 0 && mapPreviewH_ > 0) {
            w = float(mapPreviewW_); h = float(mapPreviewH_);
        }
        const float sc = std::min(kRoomColW / w, maxH / h);
        return {x, y, std::round(w * sc), std::round(h * sc)};
    }

    void GameView::drawRoomPreview(const SDL_FRect& box, bool authored) {
        const auto& room = mpRoom();
        syncRoomPreview();
        SDL_SetRenderDrawColor(ren_, 18, 20, 28, 255); SDL_RenderFillRectF(ren_, &box);
        if (mapPreviewTex_ && mapPreviewW_ > 0) {
            const float sc = std::min(box.w / float(mapPreviewW_), box.h / float(mapPreviewH_));
            const float iw = mapPreviewW_ * sc, ih = mapPreviewH_ * sc;
            const SDL_FRect dst{box.x + (box.w - iw) / 2, box.y + (box.h - ih) / 2, iw, ih};
            SDL_RenderCopyF(ren_, mapPreviewTex_, nullptr, &dst);
            // Each start in the colour of the slot that opens there. An authored scenario
            // places its own forces, so its starts belong to nobody.
            std::vector<int> colors;
            if (!authored)
                for (int slot : tak::startmarkers::occupants(room.opts, room.slots, mapPreviewStarts_.size()))
                    colors.push_back(slot >= 0 ? room.slots[slot].color : -1);
            drawStartMarkers(dst, colors);
            roomPreviewColors_ = std::move(colors);
            if (!genPreviewPending_) {
                lobbyHots_.push_back({box, [this] { openBigMap(); }});   // the map, full window
                drawClickHint(box);
            }
        }
        SDL_SetRenderDrawColor(ren_, 70, 76, 96, 255); SDL_RenderDrawRectF(ren_, &box);
    }

    // Under the pointer, a preview that opens the full-window map says so: CLICK on a
    // dark band across its middle.
    void GameView::drawClickHint(const SDL_FRect& box) {
        if (!lbHot(box)) return;
        const char* note = "CLICK";
        const SDL_FRect band{box.x, box.y + box.h / 2 - 12, box.w, 24};
        SDL_SetRenderDrawColor(ren_, 12, 14, 20, 190); SDL_RenderFillRectF(ren_, &band);
        blockText(note, box.x + (box.w - blockWidth(note, 1.6f)) / 2, band.y + 6, 1.6f, {225, 230, 240, 255});
    }

    void GameView::openBigMap() {
        // The map of the preview on show: the room's (its verified package if it has one,
        // as drawRoomPreview reads it) or the CHANGE MAP panel's candidate. A downloaded map
        // reads through a view holding its archive's files; a generated one is rolled again
        // from its id, through the base data only (as the game builds it).
        const bool room = mapPreviewFor_.rfind("room:", 0) == 0;
        std::string path = room ? mpRoom().mapId : picker_.open ? (picker_.random ? picker_.gen : picker_.file).path : std::string();
        if (path.empty()) return;
        try {
            if (tak::mapgen::isGeneratedMapId(path)) {
                const auto params = tak::mapgen::parseMapId(path);
                auto view = std::make_unique<tak::hpi::Vfs>(&vfs_, true);
                // The preview's roll when it is this map (it always is once the preview shows).
                auto map = genPreviewMap_ && genPreviewMapId_ == path ? *genPreviewMap_
                                                                      : tak::mapgen::generate(params, *view).map;
                bigMap_ = std::make_unique<BigMapView>(ren_, std::move(view), std::move(map),
                                                       tak::mapgen::worldKey(params.mapType));
            } else {
                auto view = std::make_unique<tak::hpi::Vfs>(&vfs_);
                const auto pkg = room ? mp_->mapPackage() : nullptr;
                if (pkg && pkg->files) { path = pkg->mapPath; view->setMapFiles(pkg->files); }
                else {
                    if (room) path = tak::hpi::findMap(vfs_, path);
                    if (const auto cached = vfs_.cachedMap(path)) { path = cached->first; view->setMapFiles(cached->second); }
                }
                std::string ota;
                if (auto o = view->tryRead(tak::vpath::replaceExtension(path, ".ota"))) ota.assign(o->begin(), o->end());
                std::string world = otaField(ota, "kingdom");
                std::transform(world.begin(), world.end(), world.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                bigMap_ = std::make_unique<BigMapView>(ren_, std::move(view), path, world);
            }
            bigMapFor_ = mapPreviewFor_;
            lbField_ = 0; SDL_StopTextInput();
        } catch (const std::exception&) {
            bigMap_.reset();   // an unreadable map keeps the small preview only
        }
    }

    void GameView::drawBigMap(int winW, int winH) {
        // Another map or another screen than the one it was opened from: close.
        if (bigMapFor_ != mapPreviewFor_) { bigMap_.reset(); return; }
        // Only BACK is live (the lobby drew its own buttons underneath).
        lobbyHots_.clear();
        std::fill(std::begin(genSliderRect_), std::end(genSliderRect_), SDL_FRect{}); mapThumbRect_ = {};
        const float s = lobbyScale_, lw = winW / s, lh = winH / s;
        SDL_SetRenderDrawColor(ren_, 8, 9, 13, 255);
        const SDL_FRect all{0, 0, float(winW), float(winH)};
        SDL_RenderFillRectF(ren_, &all);
        // The map fills the window above a footer as tall as the lobby's (BACK at 40 from
        // the bottom), with the lobby's 16 margin round it; a generated map's recipe takes
        // a line or two between them.
        const bool fromRoom = bigMapFor_.rfind("room:", 0) == 0;
        const std::string mapId = fromRoom ? mpRoom().mapId : picker_.open ? (picker_.random ? picker_.gen : picker_.file).id : std::string();
        constexpr float kRecipePx = 1.1f, kRecipePitch = 13;
        const auto recipe = packItems(genRecipeItems(mapId, genPreviewMountainsFor(mapId)), kRecipePx, lw - 2 * kCreateMargin);
        const float recipeH = recipe.empty() ? 0 : float(recipe.size()) * kRecipePitch + 4;
        const SDL_FRect area{kCreateMargin * s, kCreateMargin * s, (lw - 2 * kCreateMargin) * s,
                             (lh - kCreateMargin - 50 - recipeH) * s};
        const SDL_FRect r = bigMap_->draw(area);
        if (r.w > 0 && mapPreviewWorldW_ > 0) {
            // The starts as the preview shows them, in the room's colours there.
            const SDL_FRect img{r.x, r.y, mapPreviewWorldW_ * r.w / bigMap_->worldW(),
                                mapPreviewWorldH_ * r.h / bigMap_->worldH()};
            drawStartMarkers(img, fromRoom ? roomPreviewColors_ : std::vector<int>{}, 16 * s);
        }
        // The footer in the lobby's units, from the window's corner: lobbyMouse reads
        // the pointer the same way while this is open.
        lobbyOffX_ = lobbyOffY_ = 0;
        SDL_RenderSetScale(ren_, s, s);
        lbBtn(kCreateMargin, lh - 40, 120, 30, "BACK", true, [this] { bigMap_.reset(); });
        for (size_t k = 0; k < recipe.size(); ++k)
            blockText(recipe[k], (lw - blockWidth(recipe[k], kRecipePx)) / 2,
                      lh - 50 - recipeH + 2 + float(k) * kRecipePitch, kRecipePx, {205, 210, 225, 255});
        const std::string title = mapTitle(mapId);
        const std::string fitted = tak::uiTextFit(title, 1.8f, lw - 2 * (kCreateMargin + 140), false);
        blockText(fitted, (lw - blockWidth(fitted, 1.8f)) / 2, lh - 32, 1.8f, {200, 205, 220, 255});
        SDL_RenderSetScale(ren_, 1.0f, 1.0f);
    }

    void GameView::lobbyMouse(float& mx, float& my) const {
        mx = mouseX_ / lobbyScale_ - lobbyOffX_;
        my = mouseY_ / lobbyScale_ - lobbyOffY_;
    }

    // What the lobby shows under the pointer: the screen (connecting, the game list or a
    // room on its way, a room, the end of the connection) and whatever lies over it.
    uint32_t GameView::lobbyScreenNow() const {
        using S = tak::net::MpClient::State;
        const S st = mp_ ? mp_->state() : S::Offline;
        const bool room = st == S::InRoom;
        uint32_t screen = 1 + uint32_t(st);
        for (const uint32_t over : {st == S::Lobby ? uint32_t(browserWin_.kind) : 0u, uint32_t(room && modsScreen_),
                                    uint32_t(room && picker_.open), uint32_t(room && lanLeavePrompt_),
                                    uint32_t(bigMap_ != nullptr)})
            screen = screen * 8 + over;
        return screen;
    }

    void GameView::lobbyScreenNote() {
        const uint32_t screen = lobbyScreenNow();
        if (screen != lobbyScreen_) { lobbyScreen_ = screen; ++lobbyScreenSeq_; }
    }

    // The lobby's frame is drawn: its screen is on show, from now if it is a new one.
    void GameView::lobbyScreenShown() {
        lobbyScreenNote();
        if (lobbyShownSeq_ == lobbyScreenSeq_) return;
        lobbyShownSeq_ = lobbyScreenSeq_; lobbyShownMs_ = SDL_GetTicks64();
    }

    // A press after the screen changed under the pointer, which its player cannot have aimed
    // at this screen: the rest of a double click (SDL counts the presses of one), one that
    // comes before this screen was drawn at all (an earlier press in the same batch changed
    // it: the buttons under the pointer are still the old screen's), or within
    // kLobbySettleMs of its first frame. The next single click on a screen that has stood
    // for that long is taken, and is that screen's first.
    bool GameView::lobbyPressStale(const SDL_Event& e) {
        lobbyScreenNote();
        if (lobbyScreenSeq_ != lobbyPressSeq_ &&
            (e.button.clicks > 1 || lobbyShownSeq_ != lobbyScreenSeq_ ||
             SDL_GetTicks64() - lobbyShownMs_ < kLobbySettleMs)) return true;
        lobbyPressSeq_ = lobbyScreenSeq_;
        return false;
    }

    void GameView::lobbyInput(const SDL_Event& e, int winW, int winH) {
        (void)winW; (void)winH;
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            if (genSlider_ >= 0 || lbField_ == 5) applyGenParams();
            genSlider_ = -1; mapDrag_ = false;
        }
        // A press aimed at the screen before this one is no click here (lobbyPressStale).
        if (e.type == SDL_MOUSEBUTTONDOWN && lobbyPressStale(e)) {
            mouseX_ = float(e.button.x); mouseY_ = float(e.button.y);
            std::fprintf(stderr, "lobby: press dropped (the screen under it had just changed)\n");
            return;
        }
        // The full-window map view: BACK (its only button) or Esc closes it; nothing
        // else reaches the lobby under it.
        if (bigMap_) {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) bigMap_.reset();
            else if (e.type == SDL_MOUSEMOTION) { mouseX_ = float(e.motion.x); mouseY_ = float(e.motion.y); }
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                mouseX_ = float(e.button.x); mouseY_ = float(e.button.y);
                float mx, my; lobbyMouse(mx, my);
                for (auto& hot : lobbyHots_)
                    if (ptIn(hot.rect, mx, my) && hot.action) { hot.action(); break; }
            }
            return;
        }
        // A window over the game list (CREATE's, a locked game's password).
        if (browserWin_.kind != BrowserWindow::Kind::None && browserWindowInput(e)) return;
        // The CHANGE MAP panel, then the room's own keys (the MAP SCENARIO confirm); what
        // they leave falls through to the handlers below.
        if (picker_.open && pickerInput(e)) return;   // gameview_mappicker.cpp
        if (roomInput(e)) return;                     // gameview_room.cpp
        // The mods screen: Esc (no field being typed in) goes back to the room, as DONE.
        if (modsScreen_ && e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !lbField_) {
            modsScreen_ = false;
            return;
        }
        if (e.type == SDL_MOUSEMOTION) {
            mouseX_ = float(e.motion.x); mouseY_ = float(e.motion.y);
            if (mapDrag_) setMapScrollFromThumb();   // dragging the map scrollbar
            if (genSlider_ >= 0) { float mx, my; lobbyMouse(mx, my); setGenSlider(genSlider_, mx); }
        } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
            if (genSlider_ >= 0) applyGenParams();
            mapDrag_ = false; genSlider_ = -1;
        } else if (e.type == SDL_MOUSEWHEEL) {
            float mx, my; lobbyMouse(mx, my);
            if (ptIn(mapListRect_, mx, my)) {   // scroll the map list under the cursor
                mapScroll_ -= e.wheel.y;
                clampMapScroll();
            }
            if (ptIn(chatRect_, mx, my)) chatScroll_ = std::max(0, chatScroll_ + e.wheel.y);   // up: older rows
            for (int k = 0; k < 2; ++k)   // the mods screen's two lists (clamped as they draw)
                if (modsScreen_ && ptIn(modListRect_[k], mx, my)) modScroll_[k] = std::max(0, modScroll_[k] - e.wheel.y);
        } else if (e.type == SDL_MOUSEBUTTONDOWN &&
                   (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT)) {
            mouseX_ = float(e.button.x); mouseY_ = float(e.button.y);
            const bool forward = e.button.button == SDL_BUTTON_LEFT;
            if (forward) { if (lbField_ == 5) applyGenParams(); lbField_ = 0; SDL_StopTextInput(); }
            float mx, my; lobbyMouse(mx, my);
            if (forward && ptIn(mapThumbRect_, mx, my)) { mapDrag_ = true; return; }   // grab the thumb
            for (int gi = 0; gi < 9; ++gi)   // grab a density slider
                if (forward && ptIn(genSliderRect_[gi], mx, my)) { genSlider_ = gi; setGenSlider(gi, mx); return; }
            for (auto& hot : lobbyHots_)
                if (ptIn(hot.rect, mx, my)) {
                    if (hot.cycle) hot.cycle(forward ? 1 : -1);
                    else if (forward && hot.action) hot.action();
                    break;
                }
        } else if (e.type == SDL_TEXTINPUT && lbField_) {
            std::string* f = lbFieldBuf();
            // A new game's name and a game's password (fields 11 and 12, the browser's
            // windows) take printable ASCII up to the server's limits.
            const size_t cap = lbField_ == 11 ? tak::net::kRoomNameMax : lbField_ == 12 ? tak::net::kRoomPasswordMax : 24;
            if (f && f->size() < cap) {
                if (lbField_ == 11 || lbField_ == 12) {
                    for (unsigned char c : std::string(e.text.text))
                        if (f->size() < cap && c >= 0x20 && c <= 0x7e) *f += char(c);
                } else if (lbField_ == 5) {
                    for (unsigned char c : std::string(e.text.text))
                        if (f->size() < 24 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_')) *f += char(c);
                } else *f += e.text.text;
                if (lbField_ == 6) mapScroll_ = 0;   // a new filter starts at the top
                if (lbField_ == 7) modScroll_[0] = 0;
                if (lbField_ == 8) modScroll_[1] = 0;
            }
        } else if (e.type == SDL_KEYDOWN && lbField_) {
            if (e.key.keysym.sym == SDLK_BACKSPACE) {
                std::string* f = lbFieldBuf();
                if (f && !f->empty()) f->pop_back();
                if (lbField_ == 6) mapScroll_ = 0;
                if (lbField_ == 7) modScroll_[0] = 0;
                if (lbField_ == 8) modScroll_[1] = 0;
            }
            else if (e.key.keysym.sym == SDLK_RETURN) {
                if (lbField_ == 4 && !chatDraft_.empty()) { mp_->chat(chatDraft_); chatDraft_.clear(); }
                if (lbField_ == 5) applyGenParams();
                lbField_ = 0; SDL_StopTextInput();
            } else if (e.key.keysym.sym == SDLK_ESCAPE) { if (lbField_ == 5) applyGenParams(); lbField_ = 0; SDL_StopTextInput(); }
        }
    }

    void GameView::lbBtn(float x, float y, float w, float h, const std::string& label, bool enabled,
               std::function<void()> action, SDL_Color base, float labelPx, bool capsCentred) {
        SDL_FRect r{x, y, w, h};
        bool hot = enabled && lbHot(r);
        SDL_Color c = enabled ? (hot ? SDL_Color{90, 110, 150, 255} : base)
                              : SDL_Color{40, 42, 50, 255};
        SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, 255);
        SDL_RenderFillRectF(ren_, &r);
        SDL_SetRenderDrawColor(ren_, hot ? 180 : 90, hot ? 200 : 100, hot ? 240 : 130, 255);
        SDL_RenderDrawRectF(ren_, &r);
        // Shrink the label if it would overflow the button (keeps long captions like
        // "START LOCATIONS: BY SLOT" inside their box), measured, with 6 px clear each side.
        // Height too: a label with descenders (a mod pack's "Big Battles") keeps its ink
        // 2 px inside the border, and centres caps plus descenders; caps-only labels
        // centre their caps as always. A capital's tail (J, Q) counts as a descender
        // unless capsCentred.
        float px = tak::uiTextFitPx(label, labelPx, w - 12.0f);
        if (const float ink = 7 * px + (capsCentred ? 0 : tak::uiTextInkDescent(label, px)); ink > h - 4)
            px *= (h - 4) / ink;
        float tw = blockWidth(label, px);
        const float tail = capsCentred ? 0 : tak::uiTextInkDescent(label, px);
        blockText(label, x + (w - tw) / 2, y + (h - 7 * px - tail) / 2, px,
                  enabled ? SDL_Color{225, 230, 240, 255} : SDL_Color{110, 115, 125, 255});
        if (enabled && action) lobbyHots_.push_back({r, std::move(action)});
    }

    bool GameView::tntIndexedImage(const std::vector<uint8_t>& d, int field,
                                int& w, int& h, const uint8_t*& data) {
        auto u32 = [&](size_t o) -> uint32_t {
            return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) | (uint32_t(d[o + 2]) << 16)
                 | (uint32_t(d[o + 3]) << 24);
        };
        size_t hoff = size_t(field) * 4;
        if (hoff + 4 > d.size()) return false;
        size_t p = u32(hoff);
        if (p == 0 || p + 8 > d.size()) return false;
        uint32_t iw = u32(p), ih = u32(p + 4);
        if (iw == 0 || ih == 0 || iw > 4096 || ih > 4096) return false;
        if (p + 8 + size_t(iw) * ih > d.size()) return false;
        w = int(iw); h = int(ih); data = &d[p + 8];
        return true;
    }

    std::string GameView::otaField(const std::string& ota, const char* keyLower) {
        std::string low = ota;
        for (char& c : low) c = char(std::tolower((unsigned char)c));
        std::string k = keyLower;
        for (size_t pos = 0; (pos = low.find(k, pos)) != std::string::npos; pos += k.size()) {
            if (pos > 0 && (std::isalnum((unsigned char)low[pos - 1]) || low[pos - 1] == '_')) continue;
            size_t e = pos + k.size();
            while (e < low.size() && (low[e] == ' ' || low[e] == '\t')) ++e;
            if (e >= low.size() || low[e] != '=') continue;
            size_t a = e + 1, end = ota.find_first_of(";\r\n", a);
            if (end == std::string::npos) end = ota.size();
            std::string v = ota.substr(a, end - a);
            size_t s = v.find_first_not_of(" \t"), t = v.find_last_not_of(" \t");
            return s == std::string::npos ? std::string() : v.substr(s, t - s + 1);
        }
        return {};
    }
