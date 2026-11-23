// stream_key_test -- the streaming panel's REMEMBER KEY against a FAKE credential store.
// It never touches the real store (credstore_test does that, under a test-only name).
//
// What it holds the feature to (docs/streaming.md):
//   * nothing reaches the store until the panel opens, unless the box was ticked last time;
//   * ticking stores the field, pasting/starting/closing while ticked rewrites it, Ctrl+A
//     (an empty field) deletes it, unticking deletes it (a refused delete re-ticks the box);
//     nothing is stored while unticked;
//   * a ticked box loads the stored key at startup, without overwriting a typed key;
//   * no store = the box cannot be ticked and the preference does not change;
//   * a store that blocks (an unlock prompt) never blocks the panel;
//   * failures show fixed text, never the key;
//   * the serialized settings hold the preference and never the key.

#include "client/credstore.h"
#include "client/settings.h"
#include "client/streaming.h"
#include "client/streamkey.h"

#include <SDL.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace tak;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

static const std::string kDummy = "dummy-stream-key-abcd-1234-efgh";
static const std::string kOther = "dummy-other-key-zzzz-9999";

// Records every call in order. `gate` holds every call until opened (a prompt).
class Fake final : public credstore::Store {
public:
    std::string reason, contents, failWith;
    bool gated = false;
    std::string name() const override { return "FAKE STORE"; }
    std::string probe() override { wait(); note("probe"); return reason; }
    credstore::Result load(std::string& s, std::string& e) override {
        wait(); note("load");
        if (!failWith.empty()) { e = failWith; return credstore::Result::Failed; }
        if (contents.empty()) return credstore::Result::Missing;
        s = contents;
        return credstore::Result::Ok;
    }
    bool store(const std::string& s, std::string& e) override {
        wait(); note("store:" + s);
        if (!failWith.empty()) { e = failWith; return false; }
        contents = s;
        return true;
    }
    bool erase(std::string& e) override {
        wait(); note("erase");
        if (!failWith.empty()) { e = failWith; return false; }
        contents.clear();
        return true;
    }
    std::vector<std::string> take() { std::lock_guard lk(m_); auto v = log_; log_.clear(); return v; }
    void open() { std::lock_guard lk(m_); gated = false; cv_.notify_all(); }
private:
    void note(std::string s) { std::lock_guard lk(m_); log_.push_back(std::move(s)); }
    void wait() { std::unique_lock lk(m_); cv_.wait(lk, [&] { return !gated; }); }
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::string> log_;
};

using Log = std::vector<std::string>;

static std::string show(const Log& l) {
    std::string s;
    for (auto& e : l) s += (s.empty() ? "" : ", ") + e;
    return "[" + s + "]";
}
static void expect(Fake& f, const Log& want, const char* what) {
    const Log got = f.take();
    check(got == want, what);
    if (got != want) std::printf("    got %s, want %s\n", show(got).c_str(), show(want).c_str());
}

constexpr int W = 1280, H = 720;
static void key(Streaming& p, SDL_Keycode k, Uint16 mod = 0) {
    SDL_Event e{}; e.type = SDL_KEYDOWN; e.key.keysym.sym = k; e.key.keysym.mod = mod; p.input(e, W, H);
}
static void type(Streaming& p, const char* text) {
    SDL_Event e{}; e.type = SDL_TEXTINPUT; SDL_strlcpy(e.text.text, text, sizeof e.text.text); p.input(e, W, H);
}
static void clickRemember(Streaming& p) {
    const SDL_FRect r = p.rememberRect(W, H);
    SDL_Event e{}; e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_LEFT;
    e.button.x = int(r.x + r.w / 2); e.button.y = int(r.y + r.h / 2);
    p.input(e, W, H);
}
static void clickLocalCopy(Streaming& p) {
    const SDL_FRect r = p.localCopyRect(W, H);
    SDL_Event e{}; e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_LEFT;
    e.button.x = int(r.x + r.w / 2); e.button.y = int(r.y + r.h / 2);
    p.input(e, W, H);
}
static void settle(Streaming& p) { p.waitKeyStore(3000); }

int main() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    SDL_Window* win = SDL_CreateWindow("stream key test", 0, 0, W, H, SDL_WINDOW_HIDDEN);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE) : nullptr;
    if (!ren) { std::fprintf(stderr, "%s\n", SDL_GetError()); return 1; }

    std::vector<std::string> savedTexts;
    auto everSaved = [&](const std::string& needle) {
        for (auto& t : savedTexts) if (t.find(needle) != std::string::npos) return true;
        return false;
    };

    std::printf("stream_key_test: panel, store available, box off\n");
    {
        auto fake = std::make_shared<Fake>();
        Settings s;
        int saves = 0;
        Streaming p(ren, &s, [&] { savedTexts.push_back(serializeSettings(s)); ++saves; }, fake);
        p.draw(W, H);
        settle(p);
        expect(*fake, {}, "nothing touches the store before the panel opens");
        key(p, SDLK_F9); settle(p); p.draw(W, H);
        expect(*fake, {"probe"}, "opening the panel probes (no load while off)");
        SDL_SetClipboardText(kDummy.c_str());
        key(p, SDLK_v, KMOD_LCTRL); key(p, SDLK_ESCAPE); key(p, SDLK_F9); settle(p);
        expect(*fake, {}, "paste and close while off store nothing");
        clickRemember(p); settle(p);
        expect(*fake, {"store:" + kDummy}, "ticking stores the pasted key");
        check(s.streamRememberKey && saves == 1, "ticking sets and saves the preference");
        check(!savedTexts.empty() && savedTexts.back().find("streamRememberKey = 1") != std::string::npos,
              "saved settings say streamRememberKey = 1");
        key(p, SDLK_a, KMOD_LCTRL); settle(p);
        expect(*fake, {"erase"}, "Ctrl+A while ticked deletes the stored key");
        type(p, "abc"); type(p, "def"); settle(p);
        expect(*fake, {}, "typing is not written per keystroke");
        key(p, SDLK_ESCAPE); settle(p);
        expect(*fake, {"store:abcdef"}, "closing the panel writes the typed key");
        key(p, SDLK_F9); settle(p); key(p, SDLK_ESCAPE); settle(p);
        expect(*fake, {}, "reopening and closing unchanged writes nothing");
        key(p, SDLK_F9); key(p, SDLK_v, KMOD_LCTRL); settle(p);
        expect(*fake, {"store:" + kDummy}, "pasting while ticked stores at once");
        clickRemember(p); settle(p);
        expect(*fake, {"erase"}, "unticking deletes the stored key");
        check(!s.streamRememberKey && saves == 2, "unticking clears and saves the preference");
        check(savedTexts.back().find("streamRememberKey = 0") != std::string::npos,
              "saved settings say streamRememberKey = 0");
        SDL_SetClipboardText(kOther.c_str());
        key(p, SDLK_v, KMOD_LCTRL); key(p, SDLK_ESCAPE); settle(p);
        expect(*fake, {}, "after unticking, paste and close store nothing");
        check(fake->contents.empty(), "the fake store is left empty");
    }

    std::printf("stream_key_test: panel, ticked last time, key stored\n");
    {
        auto fake = std::make_shared<Fake>();
        fake->contents = kDummy;
        Settings s;
        s.streamRememberKey = true;
        Streaming p(ren, &s, [&] { savedTexts.push_back(serializeSettings(s)); }, fake);
        settle(p);
        expect(*fake, {"probe", "load"}, "startup probes and loads in the background");
        key(p, SDLK_F9); settle(p); p.draw(W, H);
        expect(*fake, {}, "opening the panel does not load again");
        clickRemember(p); settle(p);
        expect(*fake, {"erase"}, "unticking deletes the loaded key");
        clickRemember(p); settle(p);
        expect(*fake, {"store:" + kDummy}, "ticking again stores the key the field was pre-filled with");
        type(p, "x");   // the field now differs; quitting with the panel open writes it
    }
    std::printf("stream_key_test: the store refuses to delete (a cancelled unlock prompt)\n");
    {
        auto fake = std::make_shared<Fake>();
        fake->contents = kDummy;
        Settings s;
        s.streamRememberKey = true;
        int saves = 0;
        Streaming p(ren, &s, [&] { savedTexts.push_back(serializeSettings(s)); ++saves; }, fake);
        settle(p);
        key(p, SDLK_F9); settle(p); p.draw(W, H);
        fake->take();
        fake->failWith = "FAKE STORE REFUSED";
        clickRemember(p); settle(p); p.draw(W, H);
        expect(*fake, {"erase"}, "unticking asks the store to delete");
        check(s.streamRememberKey && saves == 2 && savedTexts.back().find("streamRememberKey = 1") != std::string::npos,
              "a refused delete ticks the box again and saves that");
        fake->failWith.clear();
        SDL_Event r{}; r.type = SDL_MOUSEBUTTONDOWN; r.button.button = SDL_BUTTON_RIGHT;
        const SDL_FRect rr = p.rememberRect(W, H);
        r.button.x = int(rr.x + rr.w / 2); r.button.y = int(rr.y + rr.h / 2);
        p.input(r, W, H); settle(p);
        expect(*fake, {}, "a right click does not toggle the box");
        clickRemember(p); settle(p); p.draw(W, H);
        expect(*fake, {"erase"}, "unticking again deletes");
        check(!s.streamRememberKey && fake->contents.empty(), "...and the box stays unticked");
    }
    std::printf("stream_key_test: quitting with the panel open\n");
    {
        auto fake = std::make_shared<Fake>();
        Settings s;
        s.streamRememberKey = true;
        {
            Streaming p(ren, &s, {}, fake);
            settle(p);
            key(p, SDLK_F9); type(p, "typed-before-quit"); settle(p);
            fake->take();
        }
        expect(*fake, {"store:typed-before-quit"}, "destruction writes the open panel's key first");
    }

    std::printf("stream_key_test: no system store\n");
    {
        auto fake = std::make_shared<Fake>();
        fake->reason = credstore::kNoStore;
        Settings s;
        int saves = 0;
        Streaming p(ren, &s, [&] { ++saves; }, fake);
        key(p, SDLK_F9); settle(p); p.draw(W, H);
        SDL_SetClipboardText(kDummy.c_str());
        key(p, SDLK_v, KMOD_LCTRL);
        clickRemember(p); settle(p);
        expect(*fake, {"probe", "probe"}, "greyed out: a click only probes again");
        check(!s.streamRememberKey && saves == 0, "greyed out: the preference does not change");
        key(p, SDLK_ESCAPE); settle(p);
        expect(*fake, {}, "greyed out: closing stores nothing");
    }
    {
        auto fake = std::make_shared<Fake>();
        fake->reason = credstore::kNoStore;
        Settings s;
        s.streamRememberKey = true;
        Streaming p(ren, &s, {}, fake);
        settle(p);
        expect(*fake, {"probe"}, "ticked but no store: startup probes and never loads");
        key(p, SDLK_F9); settle(p); fake->take();
        clickRemember(p); settle(p);
        check(s.streamRememberKey, "ticked but no store: the box cannot be unticked");
    }

    std::printf("stream_key_test: a store that blocks (unlock prompt)\n");
    {
        auto fake = std::make_shared<Fake>();
        fake->contents = kDummy;
        fake->gated = true;
        Settings s;
        s.streamRememberKey = true;
        const auto t0 = std::chrono::steady_clock::now();
        {
            Streaming p(ren, &s, {}, fake);
            key(p, SDLK_F9);
            for (int i = 0; i < 10; ++i) p.draw(W, H);
            key(p, SDLK_ESCAPE);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            check(ms < 500, "construct, open, draw x10 and close do not wait for the store");
            check(!p.waitKeyStore(50), "the store work is still pending behind the prompt");
            fake->open();
            settle(p);
        }
        expect(*fake, {"probe", "load"}, "it completes once the prompt is answered");
    }

    std::printf("stream_key_test: logic\n");
    {
        auto fake = std::make_shared<Fake>();
        fake->contents = kDummy;
        StreamKeyMemory m(fake, true);
        m.check();
        std::string field = "typed-meanwhile";
        m.waitIdle(3000); m.poll(field);
        check(field == "typed-meanwhile", "a loaded key never overwrites a typed one");
        fake->take();
        m.sync(field); m.waitIdle(3000); m.poll(field);
        expect(*fake, {"store:typed-meanwhile"}, "...and the typed key is written at the next write point");
        fake->failWith = "FAKE STORE FAILED";
        m.sync(kOther); m.waitIdle(3000); m.poll(field);
        const std::string err = m.takeError();
        check(err == "FAKE STORE FAILED", "a failed write reports the store's fixed text");
        check(err.find(kOther) == std::string::npos && err.find(kDummy) == std::string::npos,
              "the error never contains a key");
        fake->failWith.clear(); fake->take();
        m.sync(kOther); m.waitIdle(3000); m.poll(field);
        expect(*fake, {"store:" + kOther}, "a failed write is retried at the next write point");
    }

    {
        // A locked/denied store at startup: the stored key must survive an empty field.
        auto fake = std::make_shared<Fake>();
        fake->contents = kDummy;
        fake->failWith = "FAKE STORE LOCKED";
        StreamKeyMemory m(fake, true);
        std::string field;
        m.check(); m.waitIdle(3000); m.poll(field);
        check(field.empty() && m.takeError() == "FAKE STORE LOCKED", "a failed startup load reports and fills nothing");
        fake->take();
        m.sync(field); m.waitIdle(3000); m.poll(field);
        expect(*fake, {}, "...and an empty field then never erases the unseen key");
        fake->failWith.clear();
        m.check(); m.waitIdle(3000); m.poll(field);
        expect(*fake, {"probe", "load"}, "the next panel open retries the load");
        check(field == kDummy, "...which fills the field");
    }

    std::printf("stream_key_test: SAVE A LOCAL COPY\n");
    {
        auto fake = std::make_shared<Fake>();
        Settings s;
        int saves = 0;
        std::string last;
        check(!s.streamLocalCopy, "the box starts unticked");
        {
            Streaming p(ren, &s, [&] { last = serializeSettings(s); ++saves; }, fake);
            key(p, SDLK_F9); settle(p); p.draw(W, H);
            clickLocalCopy(p);
            check(s.streamLocalCopy && saves == 1 && last.find("streamLocalCopy = 1") != std::string::npos,
                  "ticking sets and saves the preference");
            check(!s.streamRememberKey, "...and leaves REMEMBER KEY alone");
            // While a stream runs the box is greyed out: the choice applies at the next start.
            const auto file = std::filesystem::temp_directory_path() / "tak-stream-key-test.flv";
            video::StreamConfig c; c.encoder = "libx264";
            check(p.stream().startRecording(c, file.string()), "a local recording runs");
            clickLocalCopy(p); p.draw(W, H);
            check(s.streamLocalCopy && saves == 1, "the box cannot change while streaming");
            p.stream().stop();
            for (int i = 0; i < 500 && p.stream().active(); ++i) SDL_Delay(10);
            std::filesystem::remove(file);
            clickLocalCopy(p);
            check(!s.streamLocalCopy && saves == 2 && last.find("streamLocalCopy = 0") != std::string::npos,
                  "unticking after the stream clears and saves it");
            clickLocalCopy(p);
        }
        // A restart: a new panel reads the remembered choice (ticked), so a click unticks.
        Streaming again(ren, &s, [&] { last = serializeSettings(s); ++saves; }, fake);
        key(again, SDLK_F9); settle(again);
        clickLocalCopy(again);
        check(!s.streamLocalCopy && last.find("streamLocalCopy = 0") != std::string::npos,
              "the remembered choice is the box's state after a restart");
        key(again, SDLK_ESCAPE); settle(again);
    }

    check(!savedTexts.empty(), "settings were serialized");
    check(!everSaved(kDummy) && !everSaved(kOther) && !everSaved("abcdef") && !everSaved("typed"),
          "no serialized settings ever contain a key");

    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    std::printf("stream_key_test: %s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
