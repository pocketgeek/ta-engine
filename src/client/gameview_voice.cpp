#include "client/gameview.h"

// Out-of-line GameView methods for voice chat (client/voice.h). Presentation only:
// nothing here touches world_, a command or a replay. The engine relays frames through
// mp_ on their own message types; the diplomacy VOICE mask rides on every frame.

    bool GameView::voiceRoomActive() const {
        // Single-player and campaign games run on a private local server with nobody to
        // talk to; replays and benchmark runs never open the microphone.
        if (!mp_ || replayMode_ || benchmarkMode_ || singlePlayer_ || !missionStem_.empty()) return false;
        using S = tak::net::MpClient::State;
        const S st = mp_->state();
        return st == S::InRoom || st == S::Starting || st == S::InGame;
    }

    bool GameView::textEntryActive() const {
        // Every text field in the client starts SDL text input when it takes focus and
        // stops it when it lets go (main() stops the input SDL starts by default), so that
        // is the one signal that covers all of them; the composers we own are checked too.
        // A lobby field only counts in the lobby: one still marked when the game starts is
        // not being typed in (startMpGame lets go of it as well).
        return SDL_IsTextInputActive() || chatTyping_ || (lbField_ != 0 && inLobbyPhase()) ||
               (options_ && options_->editingText());
    }

    bool GameView::voiceKey(const SDL_Event& e) {
        if ((e.type != SDL_KEYDOWN && e.type != SDL_KEYUP) || !voice_ || hotkeysScreen_) return false;
        // Voice activation needs no key: the push-to-talk key is left to everything else.
        if (settings_ && settings_->voiceActivation) { ptt_.release(); return false; }
        if (e.type == SDL_KEYUP) return hotkeys_.pushToTalkRelease(e.key.keysym.sym) && ptt_.release();
        if (!hotkeys_.pushToTalkPress(e.key.keysym.sym, e.key.keysym.mod)) return false;
        return ptt_.press(e.key.repeat != 0, voiceRoomActive() && voice_->enabled() && !spectating_, textEntryActive());
    }

    void GameView::pumpVoice() {
        if (!voice_) return;
        if (settings_) voice_->configure(*settings_, tak::currentAudioDevice());
        const bool room = voiceRoomActive();
        const bool on = room && voice_->enabled();
        if (mp_) {
            mp_->setVoiceListen(on);
            for (auto& f : mp_->takeVoice()) {
                if (!on) continue;
                tak::VoiceEngine::Incoming in;
                in.talker = f.talker; in.slot = f.slot; in.seq = f.seq; in.flags = f.flags;
                in.name = std::move(f.name); in.opus = std::move(f.opus);
                voice_->push(std::move(in));
            }
        }
        // Never leak a held key: a release we did not see (the key is up), the window
        // losing focus, a text field taking it, or talking no longer being allowed all
        // end the transmission at once.
        bool down = false;
        if (const tak::KeyChord k = hotkeys_.chord(tak::Act::PushToTalk); k.bound()) {
            const SDL_Scancode sc = SDL_GetScancodeFromKey(SDL_Keycode(k.key));
            const Uint8* keys = SDL_GetKeyboardState(nullptr);
            down = sc != SDL_SCANCODE_UNKNOWN && keys && keys[sc];
        }
        SDL_Window* win = SDL_RenderGetWindow(ren_);
        const bool focused = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS);
        const bool activation = settings_ && settings_->voiceActivation;
        // A spectator listens but never talks in a running game (the server drops it too).
        const bool talk = on && !spectating_;
        ptt_.frame(talk && !hotkeysScreen_ && !activation, textEntryActive(), focused, down);
        // The diplomacy VOLUME sliders, by slot. They belong to the game: in a room the
        // slots may hold other people, so everyone plays at 100 there.
        const bool inGame = mp_ && mp_->state() == tak::net::MpClient::State::InGame;
        for (int p = 0; p < 8; ++p) voice_->setPlayerVolume(p, inGame ? voiceVolume_[size_t(p)] : 100);
        // Before the game runs the server sends to the whole room whatever the mask says,
        // as it does chat.
        const uint8_t mask = voiceRecipients_;
        const uint8_t spectators = voiceSpectators_ ? tak::net::kVoiceSpectators : 0;
        // Voice activation: allowed whenever voice is on in the room; the engine's gate
        // opens on the microphone's level, typing or not, focused or not.
        voice_->update(room, activation ? talk : ptt_.talking(), [this, mask, spectators](const tak::VoiceEngine::Frame& f) {
            if (mp_) mp_->sendVoice(f.seq, mask, uint8_t(f.flags | spectators), f.opus);
        });
    }

    bool GameView::voiceVolumeSlider(int p) const {
        // Humans only (an AI has no voice), and never yourself (you never hear yourself).
        return p >= 0 && p < 8 && p != localPlayer_ && (humanSlots_ & (1u << p));
    }

    void GameView::setVoiceVolumeAt(const SDL_FRect& track, int p, float mx) {
        if (p < 0 || p >= 8 || track.w <= 0) return;
        const float frac = std::clamp((mx - track.x) / track.w, 0.0f, 1.0f);
        voiceVolume_[size_t(p)] = uint8_t(std::lround(frac * 100.0f));
    }

    void GameView::openVoiceOptions() {
        if (!settings_) return;
        tak::VoicePageHooks hooks;
        if (voice_) {
            hooks.micLevel = [this] { return voice_->micLevel(); };
            hooks.micNote = [this] { return tak::VoiceEngine::note(voice_->micState()); };
            hooks.outNote = [this] { return tak::VoiceEngine::note(voice_->outState()); };
            hooks.monitor = [this](bool on) { voice_->setMonitoring(on); };
        }
        // pumpVoice() applies every change on the next frame; each is saved as made.
        options_ = std::make_unique<tak::OptionsScreen>(ren_, *settings_, [] {},
            [this] { tak::saveSettings(*settings_); }, std::move(hooks), [this] { openHotkeys(); });
    }

    // Who is talking, top-left, in the HUD's own block style: YOU while you transmit (MIC
    // OFF while you hold push to talk with the MICROPHONE switched off), then each player
    // heard in the last moment in their colour, with a small speaker glyph.
    void GameView::drawVoiceIndicator(int winW, int winH) {
        (void)winH;
        if (!voice_ || !voice_->enabled() || !voiceRoomActive()) return;
        struct Row { std::string text; SDL_Color color; };
        std::vector<Row> rows;
        if (voice_->transmitting()) rows.push_back({"YOU", {255, 200, 110, 255}});
        else if (settings_ && !settings_->voiceMicEnabled && !settings_->voiceActivation && ptt_.talking())
            rows.push_back({"MIC OFF", {150, 155, 165, 255}});
        for (const auto& t : voice_->speaking()) {
            std::string name = t.name;
            if (name.empty() && t.slot < 8) name = playerName_[t.slot];
            if (name.empty()) name = t.slot < 8 ? "PLAYER " + std::to_string(t.slot + 1) : "SPECTATOR";
            SDL_Color c = t.slot < 8 ? playerColor(t.slot) : SDL_Color{200, 205, 215, 255};
            rows.push_back({name, c});
        }
        if (rows.empty()) return;
        const float px = 1.8f, rowH = 7 * px + 8, icon = 7 * px + 10;
        float w = 0;
        for (auto& r : rows) {
            r.text = tak::uiTextFit(r.text, px, 20 * 6 * px);   // a player's own capitals under MODERN
            w = std::max(w, blockWidth(r.text, px));
        }
        const float x = 12, y0 = topLeft_.place(panelTex_ ? 7.0f : 55.0f, float(rows.size()) * rowH + 6) + 5;
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        SDL_FRect bg{x - 6, y0 - 5, std::min(w + icon + 14, float(winW) - x), float(rows.size()) * rowH + 6};
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 140);
        SDL_RenderFillRectF(ren_, &bg);
        float y = y0;
        for (const auto& r : rows) {
            // Speaker: a body, a cone, and two short waves.
            const float cy = y + 7 * px / 2;
            SDL_SetRenderDrawColor(ren_, r.color.r, r.color.g, r.color.b, 255);
            SDL_FRect body{x, cy - 2, 4, 5};
            SDL_RenderFillRectF(ren_, &body);
            for (int i = 0; i < 4; ++i) {
                SDL_FRect cone{x + 4 + float(i), cy - 2 - float(i), 1, 5 + 2 * float(i)};
                SDL_RenderFillRectF(ren_, &cone);
            }
            SDL_RenderDrawLineF(ren_, x + 10, cy - 2, x + 10, cy + 3);
            SDL_RenderDrawLineF(ren_, x + 13, cy - 4, x + 13, cy + 5);
            blockText(r.text, x + icon, y, px, r.color);
            y += rowH;
        }
    }
