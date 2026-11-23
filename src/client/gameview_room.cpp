#include "client/gameview.h"
#include "net/crypto.h"
#include "net/matchconfig.h"
#include "net/roomseats.h"
#include <cmath>
#include <cstring>
#include <random>

// Out-of-line GameView method definitions (the game room): the left column -- the room's
// name and password line, the slot table, READY / START / LEAVE, the note line and the
// options block -- and the room's per-frame reactions: the host's own READY in single player,
// WATCH's AI fill, the change display (flashes and notes). The right column is
// drawRoomColumn (gameview_lobby.cpp); the CHANGE MAP panel is gameview_mappicker.cpp.

namespace {
using tak::RoomField;
const char* const kFogName[3] = {"NOT EXPLORED", "EXPLORED", "FULL VISION"};
constexpr uint16_t kUnitCaps[] = {250, 500, 1000, 2000};
constexpr SDL_Color kAmber{235, 205, 120, 255}, kRed{255, 130, 110, 255};
const char* onOff(bool value) { return value ? "ON" : "OFF"; }

// A rule's value as its cell and the chat show it.
std::string ruleValue(RoomField field, const tak::net::GameOptions& o) {
    switch (field) {
    case RoomField::Scenario: return onOff(o.mapScenario);
    case RoomField::Crusades: return onOff(o.crusades);
    case RoomField::DoubleSight: return onOff(o.doubleSight);
    case RoomField::UnitCap: return std::to_string(o.unitCap);
    case RoomField::Monarch: return onOff(o.monarchExpendable);
    case RoomField::SpeedUnlock: return onOff(o.speedUnlock);
    case RoomField::Fog: return kFogName[std::min<int>(o.fogExplored, 2)];
    case RoomField::StartLocations:
        switch (tak::net::normalizeStartMode(o.randomStarts)) {
        case tak::net::kStartsRandom: return "RANDOM";
        case tak::net::kStartsBySlot: return "BY SLOT";
        default: return "FIXED";
        }
    case RoomField::Pathfinding: {
        std::string name = tak::sim::pathfindingModeName(o.pathfindingMode);
        for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
        return name;
    }
    case RoomField::PowerCodes: return onOff(o.powerCodes);
    case RoomField::AllowSpectators: return onOff(o.allowSpectators);
    default: return "";
    }
}
std::string ruleLabel(RoomField field, const tak::net::GameOptions& o) {
    return std::string(tak::roomFieldCaption(field)) + ": " + ruleValue(field, o);
}

// The options block draws every label at one size: the largest (up to 1.6) at which every
// caption a cell can show fits `maxW`, so the block reads as one list and cycling a cell
// never resizes its text. Measured in the current UI FONT.
float roomRulePx(float maxW) {
    static const char* const kCaptions[] = {
        "CRUSADES: OFF", "DOUBLE SIGHT/RADAR: OFF", "UNIT CAP: 2000", "MONARCH EXPENDABLE: SCENARIO",
        "MONARCH EXPENDABLE: OFF", "ALLOW SPEED CHANGE: OFF", "POWER CODES: OFF", "ALLOW SPECTATORS: OFF",
        "MAP SCENARIO: OFF", "MAP SCENARIO: ON (TEST MAP)"};
    float px = 1.6f;
    tak::net::GameOptions o;
    for (auto mode : {tak::sim::PathfindingMode::Retail, tak::sim::PathfindingMode::Legion}) {
        o.pathfindingMode = mode;
        px = tak::uiTextFitPx(ruleLabel(RoomField::Pathfinding, o), px, maxW);
    }
    for (uint8_t mode = 0; mode < 3; ++mode) {
        o.randomStarts = mode; o.fogExplored = mode;
        px = tak::uiTextFitPx(ruleLabel(RoomField::StartLocations, o), px, maxW);
        px = tak::uiTextFitPx(ruleLabel(RoomField::Fog, o), px, maxW);
    }
    for (const char* caption : kCaptions) px = tak::uiTextFitPx(caption, px, maxW);
    return std::floor(px * 20) / 20;   // a 0.05 step, so rounding never re-overflows
}

// The pulsing amber fill behind a line a change lights (alpha 40..120 at 2 Hz) until `until`.
void drawPulse(SDL_Renderer* ren, const SDL_FRect& rect, uint64_t until) {
    const uint64_t now = SDL_GetTicks64();
    if (now >= until) return;
    const float wave = 0.5f + 0.5f * std::cos(float(until - now) * 0.001f * 2.0f * 6.2831853f);
    SDL_SetRenderDrawColor(ren, kAmber.r, kAmber.g, kAmber.b, uint8_t(40 + 80 * wave));
    SDL_RenderFillRectF(ren, &rect);
}

// The room plays its map's scenario: the MAP SCENARIO option, or (from the verified package)
// a Test Map copy's opt-in. It fixes START LOCATIONS and owns MONARCH EXPENDABLE.
bool scenarioPlays(const tak::net::RoomView& room, bool havePackage, const tak::sim::ScenarioInfo& s) {
    const bool fileMap = room.mission.empty() && !tak::mapgen::isGeneratedMapId(room.mapId);
    return fileMap && (room.opts.mapScenario != 0 ||
                       (havePackage && !room.opts.stressTest && !room.opts.benchmark && s.active(false)));
}

// "REMOVES 2 AI PLAYERS AND MOVES 1 PLAYER": what a capacity plan does to the seats.
std::string seatPlanText(const tak::net::SeatPlan& plan) {
    const auto count = [](size_t n, const char* one, const char* many) { return std::to_string(n) + (n == 1 ? one : many); };
    std::string out;
    if (!plan.removedAi.empty()) out = "REMOVES " + count(plan.removedAi.size(), " AI PLAYER", " AI PLAYERS");
    if (!plan.moves.empty()) out += (out.empty() ? "MOVES " : " AND MOVES ") + count(plan.moves.size(), " PLAYER", " PLAYERS");
    return out;
}

// The host's own choices start as the room's rules. A single-player host watching the AIs
// has FULL VISION and no POWER CODES forced on the room, so those two come from the
// remembered set and return when it sits down again.
void seedChosen(tak::net::GameOptions& chosen, bool& set, const tak::net::RoomView& room, bool watchingSp,
                const tak::Settings* settings) {
    if (set) return;
    chosen = room.opts;
    if (watchingSp && settings) {
        chosen.fogExplored = uint8_t(std::clamp(settings->spRoom.fog, 0, 2));
        chosen.powerCodes = settings->spRoom.powerCodes ? 1 : 0;
    }
    set = true;
}
}  // namespace

    GameView::RoomFrame GameView::roomFrame(int winW) {
        const auto& room = mp_->room();
        RoomFrame f;
        // The slot table starts just under the page title (multiplayer: under the room's
        // name). The map's name heads the right column, above its preview.
        if (!singlePlayer_) f.y += 34;
        // The right column (map, mods, chat) and the slot table's width, both from kRoomColW.
        f.colX = winW - kRoomEdge - kRoomColW;
        f.rowW = f.colX - kRoomEdge - f.x;
        f.host = (room.hostId == mp_->myClientId());
        // MAP SCENARIO (259): the room plays its map's scenario when the option is on, or
        // for a Test Map copy, read from the verified package (the host's own until then).
        // The seats that scenario needs are tagged in the slot table and must be played to START.
        if (scenarioLobbyPackage_.lock() != mp_->mapPackage()) {
            scenarioLobbyPackage_ = mp_->mapPackage();
            // With this client's registry, so the Use Only count leaves out the names no
            // unit answers to, as setupMatch does (the CHANGE MAP panel counts the same way).
            scenarioLobby_ = mp_->mapPackage() ? tak::net::maps::scenarioInfo(*mp_->mapPackage(), &registry_) : tak::sim::ScenarioInfo{};
        }
        f.roomMap = room.mission.empty();
        f.fileMap = room.mission.empty() && !tak::mapgen::isGeneratedMapId(room.mapId);
        f.scenarioOn = f.fileMap && !room.opts.stressTest && !room.opts.benchmark && mp_->mapPackage() &&
                       scenarioLobby_.active(room.opts.mapScenario != 0);
        f.needSeats = f.scenarioOn ? scenarioLobby_.requiredSeats() : 0;
        // The room's own MAP SCENARIO line reads ON from the option alone (a joiner sees it
        // before the package is verified); the rule cells that follow it agree with it.
        f.scenarioShown = f.fileMap && (room.opts.mapScenario != 0 || f.scenarioOn);
        // A multiplayer room (not single player's private one) needs two human players:
        // AI alone is single player.
        int humans = 0;
        for (const auto& s : room.slots) humans += s.type == 1;
        f.needHuman = !singlePlayer_ && humans < 2;
        return f;
    }

    // A room playing its map's scenario may start solo, once every seat the scenario needs
    // is played; ordinary skirmishes need two.
    bool GameView::roomCanStart(const RoomFrame& f) const {
        const auto& room = mp_->room();
        return f.host && startValid(room, f.scenarioOn, f.needSeats) && room.mapsReady && !mp_->modPending() &&
               (!room.opts.gameMods || bool(mp_->modPackage())) && !f.needHuman;
    }

    void GameView::roomStart() { mp_->startGame(); }

    void GameView::roomSetReady(bool ready) {
        const auto& room = mpRoom();
        if (room.mySlot < 0) { roomActError_ = "not seated"; return; }
        const auto& s = room.slots[room.mySlot];
        mp_->setSlot(room.mySlot, 1, s.faction, s.color, s.team, ready ? 1 : 0, s.aiLevel);
    }

    // The host's slot type cell (drawRoomSlots): an empty seat becomes OPEN, AI or CLOSED,
    // keeping its side, colour and team.
    void GameView::roomSetSlotType(int slot, uint8_t type) {
        const auto& room = mpRoom();
        if (!mp_->roomEditable()) { roomActError_ = "only the host changes a seat, before the game starts"; return; }
        if (slot < 0 || slot >= tak::net::kMaxSlots || room.slots[slot].type == 1) {
            roomActError_ = "not an empty seat"; return;
        }
        const auto& s = room.slots[slot];
        mp_->setSlot(slot, type, s.faction, s.color, s.team, 0, s.aiLevel);
    }

    void GameView::roomStep() {
        const auto& room = mp_->room();
        const bool host = (room.hostId == mp_->myClientId());
        const uint64_t now = SDL_GetTicks64();
        if (room.id) {   // 0: joined, and the room's first LobbyState is still on its way
            // A room seen for the first time (or over a new connection): its display starts afresh.
            if (!roomSeenSeq_ || room.id != roomSeen_.id || mp_->lobbySeq() < roomSeenSeq_) {
                roomSeen_ = room;
                roomSeenSeq_ = mp_->lobbySeq();
                std::fill(std::begin(roomFlashUntil_), std::end(roomFlashUntil_), 0);
                std::fill(std::begin(roomSeatFlashUntil_), std::end(roomSeatFlashUntil_), 0);
                roomNote_.clear(); roomRefusal_.clear(); roomNoteUntil_ = roomRefusalUntil_ = 0;
                // A remembered map passed over when the room was made: the room says why.
                if (!roomOpenNote_.empty()) {
                    roomNote_ = std::exchange(roomOpenNote_, {});
                    roomNoteUntil_ = now + 10000;
                    std::fprintf(stderr, "room note: %s\n", roomNote_.c_str());
                }
                std::fprintf(stderr, "room: game %u '%s'%s\n", room.id, room.name.c_str(),
                             room.passworded ? " (password set)" : "");
                roomOptsDraft_.reset(); scenarioArmUntil_ = 0; watchFillAt_ = 0;
                roomWatching_ = false; roomChosenSet_ = false;
            }
            // A host's own choices (a successor host's too: from the rules it inherits).
            if (!host) roomChosenSet_ = false;
            else seedChosen(roomChosen_, roomChosenSet_, room, singlePlayer_ && mp_->isSpectator(), settings_);
            // A refused map change, while the CHANGE MAP panel is closed: red on the note
            // line for 6 s (the panel shows its own refusals in its footer). A map change
            // on its way makes room for its own answer, even one worded as the last (the
            // room's own requests clear it as they send).
            if (const std::string& err = mp_->roomError(); err.empty()) {
                if (now >= roomRefusalUntil_ || mp_->roomMapPending()) { roomRefusal_.clear(); roomRefusalUntil_ = 0; }
            } else if (const std::string text = "CANNOT CHANGE THE MAP: " + err; text != roomRefusal_) {
                roomRefusal_ = text; roomRefusalUntil_ = picker_.open ? 0 : now + 6000;
            }
            // The block shows a click at once; the server's answer (or 1.5 s) replaces it.
            if (roomOptsDraft_) {
                const auto& d = *roomOptsDraft_;
                const auto& o = room.opts;
                const bool answered = d.crusades == o.crusades && d.doubleSight == o.doubleSight && d.unitCap == o.unitCap &&
                    d.monarchExpendable == o.monarchExpendable && d.speedUnlock == o.speedUnlock &&
                    d.fogExplored == o.fogExplored && d.randomStarts == o.randomStarts &&
                    d.pathfindingMode == o.pathfindingMode && d.powerCodes == o.powerCodes &&
                    d.allowSpectators == o.allowSpectators;
                if (answered || now - roomOptsDraftMs_ > 1500) roomOptsDraft_.reset();
            }
            // What the host changed since the last LobbyState: flashes, the note, chat lines.
            tak::RoomChanges changes;
            const bool newState = mp_->lobbySeq() != roomSeenSeq_;
            if (newState) {
                changes = tak::roomChanges(roomSeen_, room);
                // The host's own changes light nothing; a guest sees each one.
                if (!host && changes.bits) {
                    const uint64_t until = now + 3000;
                    for (RoomField field : changes.options) roomFlashUntil_[size_t(field)] = until;
                    if (changes.bits & tak::net::kChangeMods) roomFlashUntil_[size_t(RoomField::Mods)] = until;
                    for (int seat : changes.seats) roomSeatFlashUntil_[seat] = until;
                    if (changes.unreadied && room.mySlot >= 0) roomSeatFlashUntil_[room.mySlot] = until;
                    // Multiplayer keeps each change in the chat's history too.
                    if (!singlePlayer_) {
                        for (RoomField field : changes.options) {
                            std::string line;
                            if (field == RoomField::Map) line = "THE HOST CHANGED THE MAP TO " + mapTitle(room.mapId);
                            else if (field == RoomField::Scenario) line = std::string("THE HOST TURNED MAP SCENARIO ") + onOff(room.opts.mapScenario);
                            else line = std::string("THE HOST CHANGED ") + tak::roomFieldCaption(field) + " TO " + ruleValue(field, room.opts);
                            chatLog_.push_back({"SYSTEM", line});
                        }
                        if (changes.bits & tak::net::kChangeMods) chatLog_.push_back({"SYSTEM", "THE HOST CHANGED THE GAME MODS"});
                    }
                }
                if (const std::string note = tak::roomChangeNote(changes, host); !note.empty()) {
                    roomNote_ = note;
                    roomNoteUntil_ = now + 6000;
                    std::fprintf(stderr, "room note: %s\n", note.c_str());
                }
            }
            const bool shrank = newState && room.capacity < roomSeen_.capacity;
            if (newState) { roomSeen_ = room; roomSeenSeq_ = mp_->lobbySeq(); }
            // WATCH (single player): once the server has the host watching, an AI in every
            // OPEN seat if fewer than two AIs are seated (each a random race on its seat's
            // team, in the lowest colour no seat holds), and the watcher's rules (FULL
            // VISION, no POWER CODES). Again whenever a smaller map takes AIs away.
            const bool watching = singlePlayer_ && host && mp_->isSpectator() && room.mySlot < 0;
            if (watching && mp_->roomEditable()) {
                const bool fill = (watchFillAt_ && mp_->lobbySeq() > watchFillAt_) ||   // the first room after WATCH
                                  (!roomWatching_ && !watchFillAt_) ||                  // a room made to watch
                                  (roomWatching_ && (shrank || changes.aiRemoved > 0));
                if (fill) {
                    watchFillAt_ = 0;
                    int ais = 0;
                    for (const auto& s : room.slots) ais += s.type == 2;
                    if (ais < 2) {
                        std::mt19937 rng(uint32_t(now) ^ (room.id * 2654435761u));
                        bool held[10] = {};
                        for (const auto& s : room.slots) if ((s.type == 1 || s.type == 2) && s.color < 10) held[s.color] = true;
                        for (int i = 0; i < std::min<int>(room.capacity, tak::net::kMaxSlots); ++i) {
                            if (room.slots[i].type != 0) continue;
                            uint8_t colour = 0;
                            while (colour < 9 && held[colour]) ++colour;
                            held[colour] = true;
                            mp_->setSlot(i, 2, uint8_t(rng() % 5), colour, room.slots[i].team, 1, aiLevelEnv());
                        }
                    }
                    mp_->setGameOptions(roomEffective());
                }
                roomWatching_ = true;
            } else if (!watching) {
                // Sat down again (SIT HERE): the host's own FOG OF WAR and POWER CODES return.
                if (roomWatching_ && host && room.mySlot >= 0 && mp_->roomEditable()) mp_->setGameOptions(roomEffective());
                roomWatching_ = false;
            }
        }
        // SP: no READY button -- the player is always ready. Keep the slot marked
        // ready (once, self-limiting) so the host's START enables.
        if (singlePlayer_ && room.mySlot >= 0 && !room.slots[room.mySlot].ready) {
            const auto& s = room.slots[room.mySlot];
            mp_->setSlot(room.mySlot, 1, s.faction, s.color, s.team, 1, s.aiLevel);
        }
        pickerStep();   // the CHANGE MAP panel's answer (gameview_mappicker.cpp)
    }

    void GameView::drawRoom(int winW, int winH) {
        const auto& room = mp_->room();
        const RoomFrame f = roomFrame(winW);
        const float x = f.x, rowW = f.rowW, colX = f.colX;
        const bool host = f.host, needHuman = f.needHuman;
        const uint64_t now = SDL_GetTicks64();
        // Multiplayer: the room's name and, when it has a password, either that password
        // behind SHOW / HIDE (this client's player typed it: roomPass_) or LOCKED. Both were
        // set when the game was made (the browser's CREATE window, or CONNECT TO SERVER for a
        // LOCAL SERVER) and nobody changes them here. The line keeps to the slot table's
        // width: the name shrinks before it is cut, the same with the password hidden or
        // shown, and a shown password has the room the name leaves.
        if (!singlePlayer_) {
            constexpr float kTagPx = 1.4f, kGap = 12, kToggleW = 46;
            constexpr float kPipsW = 8 * 6 * kTagPx;   // hidden: always eight pips, whatever its length
            const bool known = room.passworded && !roomPass_.empty();
            const char* tag = known ? "PASSWORD" : "LOCKED";
            const float tagW = blockWidth(tag, kTagPx);
            // What follows the name, less the password itself: the tag, and the toggle.
            const float fixedW = !room.passworded ? 0 : known ? kGap + tagW + 6 + 8 + kToggleW : kGap + tagW;
            const float nameW = rowW - fixedW - (known ? kPipsW : 0);
            const float namePx = tak::uiTextFitPx(room.name, 2.2f, nameW, 1.6f);
            const std::string name = tak::uiTextFit(room.name, namePx, nameW);
            blockText(name, x, 54 + 7 * (2.2f - namePx), namePx, {210, 210, 220, 255});   // on the 2.2 baseline
            float passW = kPipsW, passPx = kTagPx;
            if (known && roomPassShown_) {
                // Shown: at the tag's size or smaller, then cut.
                passW = rowW - fixedW - blockWidth(name, namePx);
                const ExactText exact;   // the password's own widths
                passPx = tak::uiTextFitPx(roomPass_, kTagPx, passW, 1.1f);
            }
            float tx = x + blockWidth(name, namePx) + kGap;
            if (room.passworded) {
                blockText(tag, tx, 58, kTagPx, kAmber);
                tx += tagW + 6;
            }
            if (known) {
                constexpr SDL_Color kInk{225, 230, 240, 255};
                if (roomPassShown_) {
                    const ExactText exact;   // as it was typed, in either UI FONT
                    const std::string pass = tak::uiTextFit(roomPass_, passPx, passW);
                    blockText(pass, tx, 58 + 7 * (kTagPx - passPx), passPx, kInk);
                    tx += std::max(kPipsW, blockWidth(pass, passPx));   // SHOW / HIDE stays put for a short one
                } else {
                    tx += lbPips(tx, 58, 8, kTagPx, kInk);
                }
                lbBtn(tx + 8, 54, kToggleW, 18, roomPassShown_ ? "HIDE" : "SHOW", true,
                      [this] { roomShowPassword(!roomPassShown_); }, {60, 66, 86, 255}, 1.3f);
            }
        }
        drawRoomColumn(f, winH);   // the map, its preview, the mods and the chat
        float y = f.y;
        drawRoomSlots(f, y);
        y += 10;
        // controls (single player has no READY button: the player is always ready, roomStep)
        float bx = x;
        if (!singlePlayer_) {
            bool iAmReady = room.mySlot >= 0 && room.slots[room.mySlot].ready;
            lbBtn(x, y, 130, 30, iAmReady ? "UNREADY" : "READY", room.mySlot >= 0,
                  [this, iAmReady] { roomSetReady(!iAmReady); });
            bx = x + 142;
        }
        bool canStart = roomCanStart(f);
        // The server takes a moment to build the game: show the click landed.
        if (mp_->startPending() || mp_->starting()) lbBtn(bx, y, 130, 30, "STARTING...", true, nullptr, {70, 110, 70, 255});
        else lbBtn(bx, y, 130, 30, "START", canStart, [this] { roomStart(); }, {70, 110, 70, 255});
        // A --play-map room exists to test its one map, and a CLI game has no menu to go back
        // to: leaving either quits.
        lbBtn(bx + 142, y, 120, 30, singlePlayer_ && !canReturnToMenu_ ? "QUIT" : "LEAVE", true, [this] {
            // A LOCAL SERVER's host leaving closes the game for everyone: ask first.
            if (lanHost_ && lanOthersInRoom() > 0) { lanLeavePrompt_ = true; return; }
            roomLeave();
        });
        // Room readiness also includes game mods and other players. A
        // completed local map is not progress to display while packs change;
        // their status is already shown beside the pack list.
        // The status beside the buttons stops short of the map column, shrunk before
        // it is cut, as the refusal line below is; too long for one line even then, it
        // takes two.
        const auto status = [&](const std::string& text) {
            const float room = std::max(40.0f, colX - 12 - (bx + 280));
            const float px = tak::uiTextFitPx(text, 1.3f, room, 1.1f);
            if (blockWidth(text, px) <= room) { blockText(text, bx + 280, y + 9, px, kAmber); return; }
            auto rows = tak::uiTextWrap(text, 1.1f, room);
            for (size_t k = 2; k < rows.size(); ++k) rows[1] += " " + rows[k];
            blockText(rows[0], bx + 280, y + 3, 1.1f, kAmber);
            if (rows.size() > 1) blockText(tak::uiTextFit(rows[1], 1.1f, room), bx + 280, y + 16, 1.1f, kAmber);
        };
        const auto& mapStatus = mp_->mapStatus();
        const bool watching = singlePlayer_ && host && mp_->isSpectator() && room.mySlot < 0;
        int used = 0;
        for (const auto& s : room.slots) used += s.type == 1 || s.type == 2;
        // A successor host without the room's map cannot offer it: another map is the way on.
        if (host && mp_->hostMapMissing()) status("THIS MAP IS NOT INSTALLED HERE: CHANGE MAP");
        else if (!room.mapsReady && !mp_->modPending() && !mp_->modBusy() &&
                 mapStatus != "MAP READY" && mapStatus != "MAP VERIFIED")
            status(mapStatus.empty() ? "WAITING FOR MAP VERIFICATION" : mapStatus);
        else if (watching && !mp_->startPending() && !mp_->starting())
            status(used >= 2 ? "WATCHING THE AIS" : "ADD AI PLAYERS TO WATCH");
        else if (host && needHuman && !mp_->startPending() && !mp_->starting())
            status("NEEDS 2+ HUMAN PLAYERS");
        // The note line under the controls, one message at a time: why the server refused
        // the last START (a needed seat, setupMatch's refusal...), a refused map change,
        // the MAP SCENARIO cell's confirm, then the room's note (what the host
        // changed, the seats a smaller map took). Shrunk before it is cut: a refusal that
        // names a unit and the way out should be read whole.
        const auto noteLine = [&](const std::string& text, SDL_Color colour) {
            const float px = tak::uiTextFitPx(text, 1.2f, rowW, 0.8f);
            blockText(tak::uiTextFit(text, px, rowW), x, y + 33, px, colour);
        };
        if (!mp_->startError().empty()) noteLine("CANNOT START: " + mp_->startError(), kRed);
        else if (!roomRefusal_.empty() && now < roomRefusalUntil_) noteLine(roomRefusal_, kRed);
        else if (scenarioArmUntil_ > now) {
            const bool on = room.opts.mapScenario != 0;
            const auto plan = tak::net::planCapacity(room.slots, seatsFor(roomStarts().count, scenarioLobby_, !on));
            noteLine(std::string("CLICK AGAIN: TURNING MAP SCENARIO ") + (on ? "OFF " : "ON ") + seatPlanText(plan), kAmber);
        } else if (!roomNote_.empty() && now < roomNoteUntil_) noteLine(roomNote_, kAmber);
        // The options block: the host's rules, clickable for the host. Every rule stays
        // visible with 8 slots.
        y += 44;
        drawRoomRules(f, y);
    }

    void GameView::drawRoomSlots(const RoomFrame& f, float& y) {
        const auto& room = mp_->room();
        const float x = f.x, rowW = f.rowW;
        const bool host = f.host;
        // The host's cells take clicks only while the room is the host's to edit: never in a
        // campaign mission or a Benchmark, nor once START is on its way.
        const bool edit = host && mp_->roomEditable();
        const bool watching = singlePlayer_ && host && mp_->isSpectator() && room.mySlot < 0;
        const uint8_t needSeats = f.needSeats;
        // START LOCATIONS: BY SLOT gives slot N the map's StartPos N. A seat past the map's
        // last start opens at a fallback, worked out by the sim's own rule and tagged under
        // its name. Seats are normally capped at the start count, so this shows only on a
        // map with fewer than two starts or a scenario map with more seats than starts. A
        // Test Map scenario that places units opens its seats at them, so it gets no tags.
        std::vector<int> bySlotSpot;
        int startCount = -1;
        if (tak::net::normalizeStartMode(room.opts.randomStarts) == tak::net::kStartsBySlot &&
            room.mission.empty() && !room.opts.benchmark) {
            const RoomStarts starts = roomStarts();
            if (starts.count >= 0 && (!starts.placed || room.opts.stressTest)) {
                startCount = starts.count;
                bySlotSpot = tak::sim::startSpotsFor(tak::net::matchConfigFor(room.opts, room.slots, 0).slots,
                                                     size_t(startCount), tak::sim::StartMode::BySlot);
            }
        }
        // MOVE HERE is live only where a seat decides where, or as whom, its player starts:
        // under BY SLOT (slot N opens at the map's start N), and in a scenario whose seats
        // have armies or rules of their own. Under FIXED and RANDOM a seat picks no start
        // (the occupied seats take the starts in order, or by the match's shuffle), so the
        // button is greyed and says what would light it while the pointer is on it.
        const bool seatPicks = tak::net::normalizeStartMode(room.opts.randomStarts) == tak::net::kStartsBySlot || needSeats != 0;
        SDL_FRect moveTipAt{};   // the greyed MOVE HERE under the pointer, if any
        // slot table
        const char* typeName[4] = {"OPEN", "HUMAN", "AI", "CLOSED"};
        // Each column's start in the row: type, name, AI level, side, colour, team, READY.
        constexpr float kType = 28, kName = 92, kLevel = 156, kSide = 232, kColour = 302, kTeam = 330, kReady = 372;
        for (int i = 0; i < tak::net::kMaxSlots; ++i) {
            const auto& s = room.slots[i];
            bool mine = (i == room.mySlot);
            SDL_FRect row{x, y, rowW, 30};
            SDL_SetRenderDrawColor(ren_, mine ? 40 : 26, mine ? 48 : 30, mine ? 66 : 40, 255);
            SDL_RenderFillRectF(ren_, &row);
            // A seat the host changed flashes; this player's own row only behind its READY,
            // the line a host change clears.
            if (mine && !singlePlayer_) drawPulse(ren_, {x + kReady - 4, y + 4, row.w - 50 - kReady, 22}, roomSeatFlashUntil_[i]);
            else drawPulse(ren_, row, roomSeatFlashUntil_[i]);
            char sn[12]; std::snprintf(sn, sizeof sn, "%d", i + 1);
            blockText(sn, x + 8, y + 8, 1.8f, {150, 155, 170, 255});
            // type (host may cycle open<->closed on empty slots)
            SDL_Color tcol = s.type == 1 ? SDL_Color{200, 230, 200, 255}
                           : s.type == 2 ? SDL_Color{230, 220, 150, 255}
                                         : SDL_Color{130, 135, 150, 255};
            // Each column's text fits its column, shrinking rather than running into the next.
            auto cell = [&](const std::string& t, float cx, float cy, float px, float colW, SDL_Color c) {
                const float fpx = tak::uiTextFitPx(t, px, colW * 0.97f, px * 0.7f);
                blockText(tak::uiTextFit(t, fpx, colW), cx, cy + (px - fpx) * 3.5f, fpx, c);
            };
            cell(typeName[s.type % 4], x + kType, y + 8, 1.6f, 56, tcol);
            if (edit && s.type != 1) {
                // Host cycles an empty slot OPEN -> AI -> CLOSED. AI opponents are
                // seated (and run) on the server; this is how you set up multi-AI
                // games (including single-player vs several AIs).
                SDL_FRect tb{x + kType, y + 6, 60, 18};
                lobbyHots_.push_back({tb, [this, i, t = s.type](int direction) {
                    roomSetSlotType(i, direction > 0 ? (t == 0 ? 2 : t == 2 ? 3 : 0)
                                                     : (t == 0 ? 3 : t == 3 ? 2 : 0)); }});
            }
            // Single player has no READY column, so the side / colour / team columns move up
            // to the MOVE HERE / KICK column (row.w - 94) and the name takes the room.
            const float wide = singlePlayer_ ? row.w - 98 - (kTeam + 34) : 0.0f;
            if (s.type == 1) cell(s.name, x + kName, y + 8, 1.8f, kSide - kName - 4 + wide, {225, 228, 236, 255});
            else if ((s.type == 0 || s.type == 3) && (needSeats >> i & 1))
                cell("NEEDED BY SCENARIO", x + kName, y + 9, 1.4f, kSide - kName - 4 + wide, {235, 205, 120, 255});
            else if (s.type == 2) {
                cell(s.name.empty() ? "Computer" : s.name, x + kName, y + 8, 1.6f, kLevel - kName - 4 + wide, {210, 200, 150, 255});
                // AI difficulty (host cycles PASSIVE -> EASY -> NORMAL -> HARD -> ABSURD ->
                // DEFENSIVE -> ABSURDLY DEFENSIVE -> RETAIL: kCycle, which is not the wire
                // aiLevel order that diffName and diffCol follow). RETAIL is level 6, internally Classic: the 1999 game's own AI, driven by its
                // ai/default.txt; a new AI seat still starts NORMAL.
                constexpr int kLevels = tak::ai::kAiLevels;
                static const char* diffName[kLevels] = {"DEFENSIVE", "EASY", "NORMAL", "HARD", "ABSURD",
                                                        "ABSURDLY DEFENSIVE", "RETAIL", "PASSIVE"};
                static const SDL_Color diffCol[kLevels] = {
                    {150, 180, 210, 255},   // defensive (calm blue)
                    {150, 200, 150, 255},   // easy     (green)
                    {210, 200, 150, 255},   // normal   (neutral gold)
                    {225, 150, 140, 255},   // hard     (red)
                    {230, 120, 220, 255},   // absurd   (magenta)
                    {165, 140, 245, 255},   // absurdly defensive (violet)
                    {205, 165, 105, 255},   // retail (Classic, bronze): the retail AI's port
                    {170, 195, 175, 255}};  // passive (pale green): Easy that stays home
                static constexpr uint8_t kCycle[kLevels] = {7, 1, 2, 3, 4, 0, 5, 6};
                const uint8_t lvl = uint8_t(s.aiLevel % kLevels);
                // Two words that overrun the column stack in two smaller lines.
                constexpr float kLevelW = kSide - kLevel - 2;
                if (const char* gap = std::strchr(diffName[lvl], ' ')) {
                    cell(std::string(diffName[lvl], gap), x + kLevel + wide, y + 3, 1.1f, kLevelW, diffCol[lvl]);
                    cell(gap + 1, x + kLevel + wide, y + 13, 1.1f, kLevelW, diffCol[lvl]);
                } else
                    cell(diffName[lvl], x + kLevel + wide, y + 9, 1.4f, kLevelW, diffCol[lvl]);
                if (edit) { SDL_FRect db{x + kLevel - 2 + wide, y + 2, kLevelW + 2, 26};
                    lobbyHots_.push_back({db, [this, i](int direction) { const auto& s2 = mpRoom().slots[i];
                        const int at = int(std::find(kCycle, kCycle + kLevels, s2.aiLevel) - kCycle);
                        mp_->setSlot(i, s2.type, s2.faction, s2.color, s2.team, s2.ready,
                                     kCycle[(at + direction + kLevels) % kLevels]); }}); }
            }
            if (i >= startCount && size_t(i) < bySlotSpot.size() && bySlotSpot[size_t(i)] >= 0) {
                const int spot = bySlotSpot[size_t(i)];
                blockText("NO START " + std::to_string(i + 1) + ": " +
                          (spot < startCount ? "USES START " + std::to_string(spot + 1) : std::string("NEAR MAP CENTRE")),
                          x + kName, y + 21.5f, 1.1f, {235, 205, 120, 255});
            }
            // faction / color / team edit: your own row (the host's only while it may edit),
            // or (host) any AI row.
            bool canEdit = (mine && (!host || edit)) || (edit && s.type == 2);
            cell(factionName(s.faction), x + kSide + wide, y + 8, 1.6f, kColour - kSide - 4, {200, 205, 215, 255});
            if (canEdit) { SDL_FRect fb{x + kSide + wide, y + 6, kColour - kSide - 4, 18};
                lobbyHots_.push_back({fb, [this, i](int direction) { const auto& s2 = mpRoom().slots[i];
                    mp_->setSlot(i, s2.type, (s2.faction + direction + 5) % 5, s2.color, s2.team, s2.ready, s2.aiLevel); }}); }
            colorSwatch(x + kColour + wide, y + 5, 20, s.color, canEdit ? std::function<void(int)>([this, i](int direction) {
                // Cycle to the next colour NOT already held by another used slot --
                // landing on a taken colour just blocked READY, which was a trap.
                const auto& r2 = mpRoom();
                const auto& s2 = r2.slots[i];
                uint8_t next = s2.color;
                for (int step = 1; step <= 10; ++step) {
                    uint8_t cand = uint8_t((s2.color + direction * step + 10) % 10);
                    bool taken = false;
                    for (int k = 0; k < tak::net::kMaxSlots; ++k)
                        if (k != i && (r2.slots[k].type == 1 || r2.slots[k].type == 2) &&
                            r2.slots[k].color == cand) { taken = true; break; }
                    if (!taken) { next = cand; break; }
                }
                mp_->setSlot(i, s2.type, s2.faction, next, s2.team, s2.ready, s2.aiLevel); }) : nullptr);
            char tm[8]; std::snprintf(tm, sizeof tm, "T%d", s.team + 1);
            blockText(tm, x + kTeam + wide, y + 8, 1.8f, {200, 205, 215, 255});
            if (canEdit) { SDL_FRect teb{x + kTeam + wide, y + 6, 34, 18};
                lobbyHots_.push_back({teb, [this, i](int direction) { const auto& s2 = mpRoom().slots[i];
                    mp_->setSlot(i, s2.type, s2.faction, s2.color, uint8_t((s2.team + direction + tak::net::kMaxSlots) % tak::net::kMaxSlots), s2.ready, s2.aiLevel); }}); }
            if (s.type == 1 && !singlePlayer_) {   // SP: the player is always ready, no column
                // This player's NOT READY, while a host change that cleared it flashes, reads bright.
                const bool lit = mine && SDL_GetTicks64() < roomSeatFlashUntil_[i];
                SDL_Color rc = s.ready ? SDL_Color{130, 230, 140, 255} : lit ? SDL_Color{250, 245, 230, 255} : SDL_Color{120, 125, 135, 255};
                cell(s.ready ? "READY" : "NOT READY", x + kReady, y + 8, 1.6f, row.w - 58 - kReady, rc);
            }
            // host kick button for other humans
            if (host && s.type == 1 && !mine) {
                lbBtn(x + row.w - 54, y + 3, 50, 22, "KICK", edit, [this, i] { mp_->kick(i); });
            }
            // Any seated player may take an OPEN seat (Msg::MoveSeat) where the seat picks
            // the start (seatPicks, above). Open seats are always inside the map's capacity
            // (the server closes the rest). Not in a campaign mission, whose seats are given.
            // The seat buttons share MOVE HERE's label size.
            const float seatPx = tak::uiTextFitPx("MOVE HERE", kBtnLabelPx, 90 - 12.0f);
            if (room.mission.empty() && room.mySlot >= 0 && s.type == 0) {
                const SDL_FRect move{x + row.w - 94, y + 3, 90, 22};
                lbBtn(move.x, move.y, move.w, move.h, "MOVE HERE", seatPicks && (!host || edit),
                      [this, i] { mp_->moveSeat(i); }, {60, 66, 86, 255}, seatPx);
                if (!seatPicks && lbHot(move)) moveTipAt = move;
            }
            // Single player: WATCH on the host's own row leaves the seat to watch the AIs;
            // while watching, SIT HERE on an OPEN or AI row takes that seat back.
            if (singlePlayer_ && mine && host && room.mission.empty() && !room.opts.benchmark)
                lbBtn(x + row.w - 94, y + 3, 90, 22, "WATCH", edit, [this] { roomWatch(); }, {60, 66, 86, 255}, seatPx);
            if (watching && (s.type == 0 || s.type == 2) && i < room.capacity)
                lbBtn(x + row.w - 94, y + 3, 90, 22, "SIT HERE", edit, [this, i] { roomSitHere(i); }, {60, 66, 86, 255}, seatPx);
            y += 30;   // slot row pitch: 8 rows must leave room for the option toggles below
        }
        if (moveTipAt.w > 0) {   // beside the greyed button, inside its own row
            const char* tip = "ONLY WITH START LOCATIONS: BY SLOT";
            constexpr float kTipPx = 1.3f;
            const float tw = blockWidth(tip, kTipPx) + 12;
            const SDL_FRect box{moveTipAt.x - 4 - tw, moveTipAt.y, tw, moveTipAt.h};
            SDL_SetRenderDrawColor(ren_, 12, 14, 20, 240); SDL_RenderFillRectF(ren_, &box);
            SDL_SetRenderDrawColor(ren_, 90, 100, 130, 255); SDL_RenderDrawRectF(ren_, &box);
            blockText(tip, box.x + 6, box.y + (box.h - 7 * kTipPx) / 2, kTipPx, {225, 215, 160, 255});
        }
    }

    // ---- the options block ----
    std::vector<GameView::RoomRule> GameView::roomRules(const tak::net::RoomView& room, bool host) {
        // The host sees its click at once (the draft) until the server answers.
        const auto& o = host && roomOptsDraft_ ? *roomOptsDraft_ : room.opts;
        const bool edit = host && mp_->roomEditable();
        const bool fileMap = room.mission.empty() && !tak::mapgen::isGeneratedMapId(room.mapId);
        const bool havePackage = bool(mp_->mapPackage());
        const bool scenario = scenarioPlays(room, havePackage, scenarioLobby_);
        const bool testCopy = fileMap && havePackage && scenarioLobby_.marker && scenarioLobby_.runnable();
        // Single player's watcher plays no army: FULL VISION and no POWER CODES, forced.
        const bool watching = singlePlayer_ && host && mp_->isSpectator() && room.mySlot < 0;
        std::vector<RoomRule> rules;
        const auto add = [&](RoomField field, std::string label, bool live, bool dim, std::function<void(int)> cycle) {
            live = live && edit;
            rules.push_back({field, std::move(label), live, dim, live ? std::move(cycle) : nullptr});
        };
        const auto toggle = [&](RoomField field, bool value) {
            add(field, ruleLabel(field, o), true, false, [this, field, value](int) { roomRuleCommit(field, value ? 0 : 1); });
        };
        // A cell forced by another rule: its forced value, dimmed, not clickable.
        const auto forced = [&](RoomField field, const std::string& value) {
            add(field, std::string(tak::roomFieldCaption(field)) + ": " + value, false, true, nullptr);
        };
        toggle(RoomField::Crusades, o.crusades);
        toggle(RoomField::DoubleSight, o.doubleSight);
        add(RoomField::UnitCap, ruleLabel(RoomField::UnitCap, o), true, false, [this, cap = o.unitCap](int direction) {
            const auto at = std::find(std::begin(kUnitCaps), std::end(kUnitCaps), cap);
            const int index = at == std::end(kUnitCaps) ? 0 : int(at - std::begin(kUnitCaps));
            roomRuleCommit(RoomField::UnitCap, kUnitCaps[(index + direction + 4) % 4]);
        });
        // When OFF, losing your Monarch loses the game (retail commander rule); ON makes the
        // Monarch just another unit. A scenario owns how its game ends.
        if (scenario) forced(RoomField::Monarch, "SCENARIO");
        else toggle(RoomField::Monarch, o.monarchExpendable);
        toggle(RoomField::SpeedUnlock, o.speedUnlock);
        // FOG OF WAR: display-only (never hashed); every peer applies the same rule.
        if (watching) forced(RoomField::Fog, "FULL VISION");
        else add(RoomField::Fog, ruleLabel(RoomField::Fog, o), true, false, [this, fog = std::min<int>(o.fogExplored, 2)](int direction) {
            roomRuleCommit(RoomField::Fog, (fog + direction + 3) % 3); });
        // START LOCATIONS: FIXED = the seated slots take the map's starts in order, as in
        // retail; RANDOM = shuffled for the match; BY SLOT (not retail) = slot N takes the
        // Nth start. A scenario plays on fixed starts.
        if (scenario) forced(RoomField::StartLocations, "FIXED");
        else add(RoomField::StartLocations, ruleLabel(RoomField::StartLocations, o), true, false,
                 [this, mode = tak::net::normalizeStartMode(o.randomStarts)](int direction) {
                     roomRuleCommit(RoomField::StartLocations, (mode + direction + 3) % 3); });
        add(RoomField::Pathfinding, ruleLabel(RoomField::Pathfinding, o), true, false, [this, mode = o.pathfindingMode](int direction) {
            roomRuleCommit(RoomField::Pathfinding, int(tak::sim::cyclePathfindingMode(mode, direction))); });
        // POWER CODES (retail's battle-room box): the +codes typed in the chat box work for
        // every player. Never in a campaign mission.
        if (!room.mission.empty()) rules.push_back({RoomField::kCount, "", false, false, nullptr});
        else if (watching) forced(RoomField::PowerCodes, "OFF");
        else toggle(RoomField::PowerCodes, o.powerCodes);
        // ALLOW SPECTATORS (multiplayer): may others WATCH once the game runs.
        if (!singlePlayer_) toggle(RoomField::AllowSpectators, o.allowSpectators);
        // MAP SCENARIO (an installed or received map): changes with the map (SetRoomMap), so
        // its capacity can move seats; it waits for the verified package, which says what the
        // map's scenario needs. A Test Map copy always plays its own.
        if (fileMap) {
            const bool on = room.opts.mapScenario != 0;
            const bool live = !testCopy && !mp_->roomMapPending() && havePackage && roomStarts().count >= 0 &&
                              (on || scenarioOffered(scenarioLobby_));
            add(RoomField::Scenario, testCopy ? "MAP SCENARIO: ON (TEST MAP)" : on ? "MAP SCENARIO: ON" : "MAP SCENARIO: OFF",
                live, false, [this](int) { roomScenarioClick(); });
        }
        while (!rules.empty() && rules.back().label.empty()) rules.pop_back();
        return rules;
    }

    // Two columns of cells in reading order (an empty label keeps its place blank): framed
    // buttons for the host, plain text for guests, a forced value dimmer either way.
    void GameView::drawRoomRules(const std::vector<RoomRule>& rules, float x, float y, float colW, float pitch) {
        // The host's cells are buttons (grey while one cannot be clicked); a campaign mission
        // or a Benchmark is never the host's to edit, so its host reads the rules as a guest.
        const auto& room = mpRoom();
        const bool host = room.hostId == mp_->myClientId() && room.mission.empty() && !room.opts.benchmark;
        constexpr float kCellH = 17;
        const float cellW = colW - 4;
        const float px = roomRulePx(cellW - 12);
        for (size_t k = 0; k < rules.size(); ++k) {
            const RoomRule& rule = rules[k];
            if (rule.label.empty()) continue;
            const SDL_FRect cell{x + float(k % 2) * colW, y + float(k / 2) * pitch, cellW, kCellH};
            SDL_Color text;
            if (host) {
                const bool hot = rule.live && lbHot(cell);
                const SDL_Color fill = !rule.live ? SDL_Color{40, 42, 50, 255} : hot ? SDL_Color{90, 110, 150, 255}
                                                                                     : SDL_Color{60, 66, 86, 255};
                SDL_SetRenderDrawColor(ren_, fill.r, fill.g, fill.b, 255); SDL_RenderFillRectF(ren_, &cell);
                SDL_SetRenderDrawColor(ren_, hot ? 180 : 90, hot ? 200 : 100, hot ? 240 : 130, 255); SDL_RenderDrawRectF(ren_, &cell);
                text = rule.live ? SDL_Color{225, 230, 240, 255} : rule.dim ? SDL_Color{110, 115, 125, 255} : SDL_Color{160, 165, 178, 255};
                if (rule.live && rule.cycle) lobbyHots_.emplace_back(cell, rule.cycle);
            } else
                text = rule.dim ? SDL_Color{130, 135, 150, 255} : SDL_Color{205, 210, 225, 255};
            if (rule.field != RoomField::kCount) drawRoomFlash(rule.field, cell);   // the host changed it
            const float ty = cell.y + (kCellH - 7 * px - tak::uiTextInkDescent(rule.label, px)) / 2;
            blockText(tak::uiTextFit(rule.label, px, cellW - 12), cell.x + 6, ty, px, text);
        }
    }

    void GameView::drawRoomRules(const RoomFrame& f, float y) {
        const auto& room = mp_->room();
        const float x = f.x, rowW = f.rowW;
        const bool fileMap = f.fileMap, scenarioOn = f.scenarioOn;
        const uint8_t needSeats = f.needSeats;
        const SDL_Color infoColor{205,210,225,255};
        // Two columns of cells at a 19 pitch across the slot table's width (multiplayer
        // six rows, five for a generated map; single player five), then a MAP SCENARIO's
        // details under them. A generated map's recipe is on the preview's hover box and
        // the full-window map instead.
        constexpr float kRulePitch = 19;
        const float infoColW = std::floor(rowW / 2);
        const auto rules = roomRules(room, f.host);
        drawRoomRules(rules, x, y, infoColW, kRulePitch);
        // MAP SCENARIO's details, for a file map: what it changes and the seats it needs, in
        // reading order across two columns, three rows above the canvas edge.
        if(fileMap) {
            const float detailY = y + float((rules.size() + 1) / 2) * kRulePitch + 2;
            const bool testCopy=mp_->mapPackage() && scenarioLobby_.marker && scenarioLobby_.runnable();
            const bool wanted=room.opts.mapScenario!=0 || testCopy;
            // A Test Map copy whose only scenario content is a Use Only list: no scenario,
            // but the copy's opt-in applies the list (not in a stress or benchmark room).
            const bool testUseOnly=mp_->mapPackage() && scenarioLobby_.marker && !scenarioLobby_.hasScenario &&
                                   scenarioLobby_.useOnlyTypes>=0 && !room.opts.stressTest && !room.opts.benchmark;
            int detail=0;
            auto rule=[&](const std::string& text,bool warning=false) {
                if(detail>=6)return;
                blockText(tak::uiTextFit(text,1.2f,infoColW-8),x+float(detail%2)*infoColW,detailY+float(detail/2)*12.5f,1.2f,warning ? kAmber : infoColor);
                ++detail;
            };
            if(testUseOnly && !wanted) rule("TEST MAP: "+tak::sim::scenarioUseOnlyLine(scenarioLobby_,false));
            else if(wanted && !mp_->mapPackage()) rule("CHECKING SCENARIO...");
            else if(wanted && !scenarioOn && !room.opts.stressTest && !room.opts.benchmark) {
                const auto why=tak::sim::scenarioReason(scenarioLobby_);
                rule(why.empty() ? std::string("NO SCENARIO TO PLAY") : why.front(),true);
            } else if(scenarioOn) {
                const auto lines=tak::sim::scenarioSummary(scenarioLobby_,false);   // placed, seats, use only, endings
                bool missing=false;
                for(int i=0;i<tak::net::kMaxSlots;++i)
                    if((needSeats>>i & 1) && room.slots[i].type!=1 && room.slots[i].type!=2) missing=true;
                if(lines.size()>0) rule(lines[0].text);
                if(lines.size()>1) rule(lines[1].text,missing);
                if(lines.size()>2) rule(lines[2].text);
                if(lines.size()>3) rule(lines[3].text,lines[3].warning);
                // Placements replace Monarchs, so a seated player the scenario gives no
                // units starts with nothing.
                uint8_t empty=0;
                for(int i=0;i<tak::net::kMaxSlots;++i)
                    if((room.slots[i].type==1 || room.slots[i].type==2) && !(scenarioLobby_.placedSeats>>i & 1)) empty|=uint8_t(1u<<i);
                if(scenarioLobby_.placed>0 && empty) rule("NO PLACED UNITS: PLAYER "+tak::sim::seatList(empty),true);
            }
        }
    }

    // ---- room editing: the host's edits in the room ----
    tak::net::GameOptions GameView::roomEffective() const {
        const auto& room = mp_->room();
        tak::net::GameOptions o = room.opts;
        if (roomChosenSet_) {
            const auto& c = roomChosen_;
            o.crusades = c.crusades; o.doubleSight = c.doubleSight; o.unitCap = c.unitCap;
            o.monarchExpendable = c.monarchExpendable; o.speedUnlock = c.speedUnlock; o.fogExplored = c.fogExplored;
            o.randomStarts = c.randomStarts; o.pathfindingMode = c.pathfindingMode; o.powerCodes = c.powerCodes;
            o.allowSpectators = c.allowSpectators;
        }
        if (singlePlayer_ && room.hostId == mp_->myClientId() && mp_->isSpectator() && room.mySlot < 0) {
            o.fogExplored = 2; o.powerCodes = 0;   // WATCH
        }
        if (scenarioPlays(room, bool(mp_->mapPackage()), scenarioLobby_)) o.randomStarts = tak::net::kStartsFixed;
        return o;
    }

    void GameView::roomRuleCommit(RoomField field, int value) {
        const auto& room = mpRoom();
        if (!mp_->roomEditable()) { roomActError_ = "only the host changes the rules, before the game starts"; return; }
        seedChosen(roomChosen_, roomChosenSet_, room, singlePlayer_ && mp_->isSpectator(), settings_);
        const auto rules = roomRules(room, true);
        const auto rule = std::find_if(rules.begin(), rules.end(), [&](const RoomRule& r) { return r.field == field; });
        if (field == RoomField::Scenario || rule == rules.end()) {
            roomActError_ = std::string("this room has no ") + tak::roomFieldCaption(field) + " rule to set"; return;
        }
        if (!rule->live) { roomActError_ = std::string(tak::roomFieldCaption(field)) + " is fixed here"; return; }
        // The value must be one the cell can show.
        const bool flag = value == 0 || value == 1;
        switch (field) {
        case RoomField::UnitCap:
            if (std::find(std::begin(kUnitCaps), std::end(kUnitCaps), value) == std::end(kUnitCaps)) {
                roomActError_ = "a unit cap is 250, 500, 1000 or 2000"; return;
            }
            break;
        case RoomField::Fog: case RoomField::StartLocations:
            if (value < 0 || value > 2) { roomActError_ = "a value from 0 to 2"; return; }
            break;
        case RoomField::Pathfinding:
            if (value < 0 || value > 255 || !tak::sim::validPathfindingMode(uint8_t(value))) {
                roomActError_ = "no such pathfinding mode"; return;
            }
            break;
        default:
            if (!flag) { roomActError_ = "ON is 1 and OFF is 0"; return; }
            break;
        }
        auto& c = roomChosen_;
        switch (field) {
        case RoomField::Crusades: c.crusades = uint8_t(value); break;
        case RoomField::DoubleSight: c.doubleSight = uint8_t(value); break;
        case RoomField::UnitCap: c.unitCap = uint16_t(value); break;
        case RoomField::Monarch: c.monarchExpendable = uint8_t(value); break;
        case RoomField::SpeedUnlock: c.speedUnlock = uint8_t(value); break;
        case RoomField::Fog: c.fogExplored = uint8_t(value); break;
        case RoomField::StartLocations: c.randomStarts = uint8_t(value); break;
        case RoomField::Pathfinding: c.pathfindingMode = tak::sim::PathfindingMode(value); break;
        case RoomField::PowerCodes: c.powerCodes = uint8_t(value); break;
        case RoomField::AllowSpectators: c.allowSpectators = uint8_t(value); break;
        default: break;
        }
        roomOptsDraft_ = roomEffective();
        roomOptsDraftMs_ = SDL_GetTicks64();
        mp_->setGameOptions(*roomOptsDraft_);
        rememberRoomPrefs(field);
    }

    // The MAP SCENARIO cell: the room's map again with the other scenario value, and the
    // seats it gives from the verified package (never a local file, which can be another
    // revision of the map). Removing or moving players takes a second click within 3 s.
    void GameView::roomScenarioClick() {
        const auto& room = mpRoom();
        if (!mp_->roomEditable()) { roomActError_ = "only the host changes MAP SCENARIO, before the game starts"; return; }
        if (!room.mission.empty() || tak::mapgen::isGeneratedMapId(room.mapId)) { roomActError_ = "this map has no scenario"; return; }
        if (mp_->roomMapPending()) { roomActError_ = "the last map change is still waiting for the server"; return; }
        const int starts = roomStarts().count;
        if (!mp_->mapPackage() || starts < 0) { roomActError_ = "the map is not verified yet"; return; }
        if (scenarioLobby_.marker && scenarioLobby_.runnable()) { roomActError_ = "a Test Map copy always plays its scenario"; return; }
        const bool on = room.opts.mapScenario != 0;
        if (!on && !scenarioOffered(scenarioLobby_)) { roomActError_ = "this map offers no scenario to play"; return; }
        seedChosen(roomChosen_, roomChosenSet_, room, singlePlayer_ && mp_->isSpectator(), settings_);
        const uint8_t cap = seatsFor(starts, scenarioLobby_, !on);
        const auto plan = tak::net::planCapacity(room.slots, cap);
        // (More players than the seats it leaves: the server refuses it, and says why.)
        const uint64_t now = SDL_GetTicks64();
        if ((!plan.removedAi.empty() || !plan.moves.empty()) && scenarioArmUntil_ <= now) {
            scenarioArmUntil_ = now + 3000;   // the note line says what the next click does
            std::fprintf(stderr, "room note: CLICK AGAIN: TURNING MAP SCENARIO %s %s\n", on ? "OFF" : "ON", seatPlanText(plan).c_str());
            return;
        }
        scenarioArmUntil_ = 0;
        roomRefusal_.clear(); roomRefusalUntil_ = 0;   // room for this one's answer
        if (!mp_->setRoomMap(room.mapId, cap, !on, roomChosen_.randomStarts)) roomActError_ = mp_->roomError();
    }

    // WATCH (single player): the host leaves its seat; roomStep fills the room with AIs once
    // the server has it watching.
    void GameView::roomWatch() {
        const auto& room = mpRoom();
        if (!mp_->roomEditable()) { roomActError_ = "only the host watches, before the game starts"; return; }
        if (!singlePlayer_) { roomActError_ = "only a single-player host watches the AIs"; return; }
        if (room.mySlot < 0 || mp_->isSpectator()) { roomActError_ = "already watching"; return; }
        seedChosen(roomChosen_, roomChosenSet_, room, false, settings_);
        watchFillAt_ = mp_->lobbySeq();
        mp_->watch();
    }

    // SIT HERE: a watching host takes an OPEN seat, or an AI's (the AI goes first; the two
    // requests are in order on the one connection). roomStep then sends the host's rules.
    void GameView::roomSitHere(int slot) {
        const auto& room = mpRoom();
        if (!mp_->roomEditable()) { roomActError_ = "only the host sits down, before the game starts"; return; }
        if (!mp_->isSpectator() || room.mySlot >= 0) { roomActError_ = "not watching"; return; }
        if (slot < 0 || slot >= std::min<int>(room.capacity, tak::net::kMaxSlots) ||
            (room.slots[slot].type != 0 && room.slots[slot].type != 2)) { roomActError_ = "not an open or AI seat"; return; }
        const auto& s = room.slots[slot];
        if (s.type == 2) mp_->setSlot(slot, 0, s.faction, s.color, s.team, 0, s.aiLevel);
        mp_->moveSeat(slot);
    }

    // The password this client's player typed for a room is the room's own while in it, and
    // gone when it is left; one typed for a request that was refused or given up goes too.
    // So does one the room's table shows it no longer has: this client changes no password
    // in a room, but the server still takes SetRoomInfo from one that does, and a lock that
    // was dropped and set again is not the password typed here. (The table says only
    // whether there is one: a password changed in one step cannot be told from here.)
    void GameView::roomPasswordStep() {
        using S = tak::net::MpClient::State;
        const S st = mp_->state();
        if (st == S::InRoom) {
            if (!roomPassAsked_.empty()) {
                roomPass_ = roomPassAsked_;
                tak::crypto::wipe(roomPassAsked_);
            }
            const auto& room = mp_->room();
            if (room.id && !room.passworded && !roomPass_.empty()) {   // (id 0: its first table is still to come)
                tak::crypto::wipe(roomPass_);
                roomPassShown_ = false;
                std::fprintf(stderr, "room: password forgotten (the room has none now)\n");
            }
            return;
        }
        tak::crypto::wipe(roomPass_);
        roomPassShown_ = false;
        const bool waiting = st == S::Lobby && (roomRequestOut() || (lanJoinSent_ && mp_->joinError().empty()));
        if (!waiting) tak::crypto::wipe(roomPassAsked_);
    }

    // SHOW / HIDE beside the room's password. (The log says which, never the password.)
    void GameView::roomShowPassword(bool show) {
        if (!mpRoom().passworded || roomPass_.empty()) { roomActError_ = "this client does not know a password for the room"; return; }
        if (show == roomPassShown_) return;
        roomPassShown_ = show;
        std::fprintf(stderr, "room: password %s\n", show ? "shown" : "hidden");
    }

    // Esc lets go of an armed MAP SCENARIO click.
    bool GameView::roomInput(const SDL_Event& e) {
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !lbField_ && !modsScreen_ &&
            scenarioArmUntil_ > SDL_GetTicks64()) {
            scenarioArmUntil_ = 0;
            return true;
        }
        return false;
    }

    // Write one thing back to the room kind's remembered set: the cell the host clicked (its
    // chosen value, never WATCH's or MAP SCENARIO's override), an accepted map, the game
    // packs. Only a room this session remembers (not --play-map, a harness or a guest), and
    // never a campaign mission or Benchmark room.
    void GameView::rememberRoomPrefs(RoomField field) {
        const auto& room = mpRoom();
        if (!settings_ || !rememberRoom_ || roomKind_ == RoomKind::None || room.hostId != mp_->myClientId() ||
            !room.mission.empty() || room.opts.benchmark)
            return;
        auto& p = roomKind_ == RoomKind::SinglePlayer ? settings_->spRoom : settings_->mpRoom;
        const auto& c = roomChosen_;
        switch (field) {
        case RoomField::Crusades: p.crusades = c.crusades != 0; break;
        case RoomField::DoubleSight: p.doubleSight = c.doubleSight != 0; break;
        case RoomField::UnitCap: p.unitCap = c.unitCap; break;
        case RoomField::Monarch: p.monarchExpendable = c.monarchExpendable != 0; break;
        case RoomField::SpeedUnlock: p.speedUnlock = c.speedUnlock != 0; break;
        case RoomField::Fog: p.fog = std::min<int>(c.fogExplored, 2); break;
        case RoomField::StartLocations: p.randomStarts = tak::net::normalizeStartMode(c.randomStarts); break;
        case RoomField::Pathfinding: p.pathfindingMode = c.pathfindingMode; break;
        case RoomField::PowerCodes: p.powerCodes = c.powerCodes != 0; break;
        case RoomField::AllowSpectators: p.allowSpectators = c.allowSpectators != 0; break;
        case RoomField::Map:
            // The map keeps the last installed one; the generator's recipe is what RANDOM opens on.
            if (tak::mapgen::isGeneratedMapId(room.mapId)) { p.generated = true; p.generator = room.mapId; }
            else { p.generated = false; p.map = room.mapId; }
            break;
        case RoomField::Mods: {
            // A saved tick hidden by a same-named always-enabled pack is kept (it applies
            // again once that pack leaves always_enabled/), as the MODS screen keeps it.
            auto selection = mp_->gameModPacks();
            for (const auto& n : p.gamePacks)
                if (std::find(selection.begin(), selection.end(), n) == selection.end() &&
                    std::any_of(mp_->alwaysGameModPacks().begin(), mp_->alwaysGameModPacks().end(), [&](const std::string& a) {
                        return tak::hpi::MountSet::key(a) == tak::hpi::MountSet::key(n); }))
                    selection.push_back(n);
            p.gamePacks = std::move(selection);
            break;
        }
        default: return;
        }
        saveSettings(*settings_);
    }

    void GameView::rememberRoomSort() {
        const auto& room = mpRoom();
        if (!settings_ || !rememberRoom_ || roomKind_ == RoomKind::None || room.hostId != mp_->myClientId() ||
            !room.mission.empty() || room.opts.benchmark)
            return;
        auto& p = roomKind_ == RoomKind::SinglePlayer ? settings_->spRoom : settings_->mpRoom;
        p.mapSort = mapSort_; p.mapSortDir = mapSortDir_;
        saveSettings(*settings_);
    }

    void GameView::drawRoomFlash(RoomField field, const SDL_FRect& rect) {
        if (field < RoomField::kCount) drawPulse(ren_, rect, roomFlashUntil_[size_t(field)]);
    }
