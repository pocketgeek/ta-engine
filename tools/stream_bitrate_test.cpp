// stream_bitrate_test -- the streaming panel's bitrate against YouTube's table
// (video/youtubebitrate.h) and its UPLOAD CAN'T KEEP UP warning (client/uploadwatch.h).
//
// What it holds the feature to (docs/streaming.md):
//   * YouTube's recommended figure for the stream's short side, frame rate and codec is
//     the cap, and RECOMMENDED fills it in; ultrawide 3840x1080 is a 1080p stream;
//   * only a YouTube ingest is capped: other destinations keep the whole range;
//   * a saved bitrate above the cap is lowered to it, with a note, and saved; BITRATE
//     steps never pass it, and a lower frame rate lowers it;
//   * the warning comes on when the encoder keeps waiting for the connection (over half
//     the time, or DROPPED rising while it waits), never for a single hitch, a busy
//     computer or an encoder sending under its bitrate, and clears once that stops.
// The panel runs against a store that is never available: no real stream key is read.

#include "client/credstore.h"
#include "client/settings.h"
#include "client/streaming.h"
#include "client/uploadwatch.h"
#include "video/youtubebitrate.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

using namespace tak;
using video::StreamCodec;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

class NoStore final : public credstore::Store {
public:
    std::string name() const override { return "NO STORE"; }
    std::string probe() override { return "NO STORE IN THIS TEST"; }
    credstore::Result load(std::string&, std::string&) override { return credstore::Result::Missing; }
    bool store(const std::string&, std::string& e) override { e = "NO STORE"; return false; }
    bool erase(std::string&) override { return true; }
};

constexpr int W = 1280, H = 720;
static void click(Streaming& p, SDL_FRect r, Uint8 button = SDL_BUTTON_LEFT) {
    SDL_Event e{}; e.type = SDL_MOUSEBUTTONDOWN; e.button.button = button;
    e.button.x = int(r.x + r.w / 2); e.button.y = int(r.y + r.h / 2);
    p.input(e, W, H);
}
static void key(Streaming& p, SDL_Keycode k) {
    SDL_Event e{}; e.type = SDL_KEYDOWN; e.key.keysym.sym = k; p.input(e, W, H);
}

static void table() {
    std::printf("stream_bitrate_test: YouTube's table\n");
    using video::youtubeRecommendedKbps;
    const auto H264 = StreamCodec::H264, HEVC = StreamCodec::Hevc;
    check(youtubeRecommendedKbps(3840, 2160, 60, H264) == 50000 && youtubeRecommendedKbps(3840, 2160, 60, HEVC) == 35000 &&
          youtubeRecommendedKbps(3840, 2160, 30, H264) == 42000 && youtubeRecommendedKbps(3840, 2160, 30, HEVC) == 30000,
          "2160p: 50/35 Mbps at 60, 42/30 at 30");
    check(youtubeRecommendedKbps(2560, 1440, 60, H264) == 34000 && youtubeRecommendedKbps(2560, 1440, 60, HEVC) == 24000 &&
          youtubeRecommendedKbps(2560, 1440, 30, H264) == 21000 && youtubeRecommendedKbps(2560, 1440, 30, HEVC) == 15000,
          "1440p: 34/24 Mbps at 60, 21/15 at 30");
    check(youtubeRecommendedKbps(1920, 1080, 60, H264) == 17000 && youtubeRecommendedKbps(1920, 1080, 60, HEVC) == 12000 &&
          youtubeRecommendedKbps(1920, 1080, 30, H264) == 14000 && youtubeRecommendedKbps(1920, 1080, 30, HEVC) == 10000,
          "1080p: 17/12 Mbps at 60, 14/10 at 30");
    check(youtubeRecommendedKbps(1280, 720, 60, H264) == 8000 && youtubeRecommendedKbps(1280, 720, 30, HEVC) == 6000,
          "720p: 8 Mbps H.264, 6 HEVC");
    check(youtubeRecommendedKbps(854, 480, 60, H264) == 4000 && youtubeRecommendedKbps(640, 360, 30, HEVC) == 3000,
          "480p / 360p: 4 Mbps H.264, 3 HEVC (60 fps uses the 30 row)");
    check(youtubeRecommendedKbps(3840, 1080, 60, H264) == 17000, "ultrawide 3840x1080 is a 1080p stream");
    check(youtubeRecommendedKbps(3440, 1440, 30, H264) == 21000, "3440x1440 is a 1440p stream");
    check(youtubeRecommendedKbps(1600, 900, 60, H264) == 17000, "between rows: the row above (900 -> 1080p)");
    check(youtubeRecommendedKbps(3840, 1600, 60, H264) == 50000, "1600 high -> 2160p");
    check(youtubeRecommendedKbps(1080, 1920, 60, H264) == 17000, "upright 1080x1920: the short side");
    check(youtubeRecommendedKbps(320, 240, 30, H264) == 4000, "below 360: the 360p row");
    check(youtubeRecommendedKbps(7680, 4320, 60, HEVC) == 35000, "above 2160: the 2160p row");
    check(video::streamCodec("", 3840, 1080) == H264 && video::streamCodec("", 7680, 2160) == HEVC &&
          video::streamCodec("libx264", 7680, 2160) == H264 && video::streamCodec("hevc_nvenc", 1920, 1080) == HEVC,
          "codec: AUTOMATIC is HEVC above 4096, CPU is H.264");
    check(video::isYouTubeRecommendedKbps(17000) && video::isYouTubeRecommendedKbps(35000) &&
          !video::isYouTubeRecommendedKbps(7777) && !video::isYouTubeRecommendedKbps(80000),
          "the table's figures are recognised, others are not");

    std::printf("stream_bitrate_test: which destination is YouTube\n");
    using video::isYouTubeIngest;
    check(isYouTubeIngest(video::kYouTubeIngest), "the panel's own ingest");
    check(isYouTubeIngest("rtmp://a.rtmp.youtube.com/live2/k") && isYouTubeIngest("rtmp://b.rtmp.youtube.com/live2?backup=1/k") &&
          isYouTubeIngest("rtmps://b.rtmps.youtube.com:443/live2") && isYouTubeIngest("RTMPS://A.RTMPS.YOUTUBE.COM/live2"),
          "a / b, rtmp / rtmps, any case");
    check(!isYouTubeIngest("rtmp://youtube.com.example.net/live2") && !isYouTubeIngest("rtmp://notyoutube.com/live2") &&
          !isYouTubeIngest("rtmps://localhost:4443/live2") && !isYouTubeIngest("rtmp://live.twitch.tv/app") &&
          !isYouTubeIngest("file:/tmp/x.flv") && !isYouTubeIngest("https://www.youtube.com/"),
          "other hosts and schemes are not YouTube");
}

static void watch() {
    std::printf("stream_bitrate_test: UPLOAD CAN'T KEEP UP\n");
    constexpr uint64_t S = 1000000; // one sample, in microseconds
    {
        UploadWatch w;
        bool any = false;
        for (int i = 0; i < 20; ++i) any |= w.sample(0, S / 100, S);
        check(!any, "a healthy stream never warns");
    }
    {
        UploadWatch w;
        bool early = false;
        for (int i = 0; i < 3; ++i) early |= w.sample(uint64_t(i) * 10, S, S);
        check(!early, "the first samples of a connection are ignored");
    }
    {
        // A still scene: a VBR hardware encoder sends a fraction of its bitrate, and
        // the connection takes it at once. That is not the upload falling behind.
        UploadWatch w;
        bool any = false;
        for (int i = 0; i < 20; ++i) any |= w.sample(0, 0, S);
        check(!any, "sending little, with nothing waiting: no warning");
    }
    {
        // A busy computer: frames dropped, but the connection never makes it wait.
        UploadWatch w;
        uint64_t dropped = 0;
        bool any = false;
        for (int i = 0; i < 20; ++i) any |= w.sample(dropped += 12, S / 50, S);
        check(!any, "DROPPED rising with no wait to send: no warning");
    }
    {
        UploadWatch w;
        for (int i = 0; i < 3; ++i) w.sample(0, 0, S);
        check(!w.sample(0, S * 6 / 10, S) && !w.sample(0, S * 6 / 10, S), "waiting to send two seconds of five: not yet");
        check(w.sample(0, S * 6 / 10, S), "waiting over half of three seconds: warns");
        bool held = true;
        for (int i = 0; i < 4; ++i) held &= w.sample(0, 0, S);
        check(held, "...and holds while recovering");
        check(!w.sample(0, 0, S), "...then clears after five clear seconds");
        bool any = false;
        for (int i = 0; i < 10; ++i) any |= w.sample(0, S * 4 / 10, S);
        check(!any, "waiting under half the time, with no dropped frames: no warning");
    }
    {
        UploadWatch w;
        for (int i = 0; i < 3; ++i) w.sample(0, 0, S);
        uint64_t dropped = 0;
        bool once = false;
        once |= w.sample(dropped += 30, S / 5, S);
        for (int i = 0; i < 4; ++i) once |= w.sample(dropped, S / 5, S);
        check(!once, "a single hitch of dropped frames does not warn");
        w.sample(dropped += 5, S / 5, S);
        w.sample(dropped, S / 5, S);
        check(!w.sample(dropped += 5, S / 5, S), "DROPPED rising while waiting, two of five: not yet");
        check(w.sample(dropped += 5, S / 5, S), "DROPPED rising while waiting, three of five: warns");
        w.reset();
        check(!w.warning(), "a new connection starts clear");
    }
}

static void panel(SDL_Renderer* ren) {
    std::printf("stream_bitrate_test: panel\n");
    Settings s;
    s.streamResolution = 1; s.streamFps = 60; s.streamBitrate = 45000; s.streamCpuEncoder = false;
    int saves = 0;
    Streaming p(ren, &s, [&] { ++saves; }, std::make_shared<NoStore>());
    check(saves == 0, "nothing is saved before the panel opens");
    key(p, SDLK_F9); p.draw(W, H);
    check(s.streamBitrate == 17000 && saves == 1, "a saved 45000 at 1080p 60 is lowered to 17000 and saved");
    check(p.notice().find("17000") != std::string::npos, "...with a note saying why");
    p.draw(W, H);
    check(saves == 1, "...once");
    click(p, p.bitrateRect(W, H));
    check(s.streamBitrate == 3000 && p.notice().empty(), "BITRATE steps past the cap wrap to the lowest; the note clears");
    int highest = 0;
    for (int i = 0; i < 12; ++i) { click(p, p.bitrateRect(W, H)); highest = std::max(highest, s.streamBitrate); }
    check(highest == 17000, "stepping up never passes the cap, and reaches it");
    while (s.streamBitrate != 3000) click(p, p.bitrateRect(W, H));
    click(p, p.bitrateRect(W, H), SDL_BUTTON_RIGHT);
    check(s.streamBitrate == 17000, "stepping back from the lowest lands on the cap");
    // FRAME RATE row (the row above BITRATE): 60 -> 30 lowers the cap to 14000.
    SDL_FRect fps = p.bitrateRect(W, H); fps.y -= 38 * (fps.h / 32);
    click(p, fps);
    check(s.streamFps == 30 && s.streamBitrate == 14000, "30 fps lowers 17000 to 1080p 30's 14000");
    check(p.notice().find("14000") != std::string::npos, "...with the note");
    click(p, p.bitrateRect(W, H), SDL_BUTTON_RIGHT);
    click(p, p.bitrateRect(W, H), SDL_BUTTON_RIGHT);
    check(s.streamBitrate == 9000, "stepping down from the cap");
    click(p, p.recommendedRect(W, H));
    check(s.streamBitrate == 14000 && p.notice().empty(), "RECOMMENDED fills in 1080p 30's 14000");
    // RESOLUTION to 1440p: its 12000 floor is under 1440p 30's 21000, so nothing changes.
    SDL_FRect res = p.bitrateRect(W, H); res.y -= 76 * (res.h / 32);
    click(p, res);
    check(s.streamResolution == 2 && s.streamBitrate == 14000, "1440p keeps 14000");
    click(p, p.recommendedRect(W, H));
    check(s.streamBitrate == 21000, "...and RECOMMENDED is 1440p 30's 21000");
    click(p, res, SDL_BUTTON_RIGHT);
    check(s.streamResolution == 1 && s.streamBitrate == 14000 && !p.notice().empty(),
          "back to 1080p lowers 21000 to 14000, with the note");
    // MAX 3840 in a 7680x2160 window sends 3840x1080: 1080p, not the 4K floor of 30000.
    for (int i = 0; i < 4; ++i) click(p, res);
    check(s.streamResolution == 5, "MAX 3840");
    {
        constexpr int BW = 7680, BH = 2160;
        Settings u;
        u.streamResolution = 5; u.streamFps = 60; u.streamBitrate = 30000;
        Streaming q(ren, &u, [] {}, std::make_shared<NoStore>());
        SDL_Event f9{}; f9.type = SDL_KEYDOWN; f9.key.keysym.sym = SDLK_F9; q.input(f9, BW, BH);
        q.draw(BW, BH);
        check(u.streamBitrate == 17000, "a 7680x2160 window under MAX 3840 streams 3840x1080 at most 17000");
    }
}

int main() {
    table();
    watch();
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    SDL_Window* win = SDL_CreateWindow("stream bitrate test", 0, 0, W, H, SDL_WINDOW_HIDDEN);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE) : nullptr;
    if (!ren) { std::fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    panel(ren);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    std::printf("%s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
