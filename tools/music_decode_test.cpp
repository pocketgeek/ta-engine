// CUSTOM MUSIC decoding (client/musicfile.h): every format a player may choose decodes to
// S16 at its own rate (never above 44100), a file that is not audio is refused with a
// reason, and the custom-track registry names only the tracks that have a file.
// The fixtures (tools/testdata/music) are half-second test tones generated with ffmpeg.
#include "client/musicfile.h"
#include "hpi/hpi.h"

#include <SDL.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  %-58s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

static std::vector<uint8_t> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
    SDL_Init(0);
    const std::string dir = std::string(TA_SOURCE_DIR) + "/tools/testdata/music/";
    struct Case { const char* file; int channels, rate; };
    for (const Case& c : {Case{"tone.wav", 1, 22050}, Case{"tone.mp3", 1, 22050}, Case{"tone.ogg", 2, 44100},
                          Case{"tone48k.flac", 2, 44100}}) {
        std::string why;
        const auto pcm = ta::music::decode(slurp(dir + c.file), 44100, &why);
        check(pcm.has_value(), std::string(c.file) + " decodes" + (pcm ? "" : ": " + why));
        if (!pcm) continue;
        const double secs = double(pcm->samples.size()) / pcm->channels / pcm->rate;
        check(pcm->channels == c.channels && pcm->rate == c.rate,
              std::string(c.file) + ": " + std::to_string(pcm->channels) + " ch at " + std::to_string(pcm->rate) + " Hz");
        check(secs > 0.4 && secs < 0.7, std::string(c.file) + ": half a second");
        int peak = 0;
        for (int16_t v : pcm->samples) peak = std::max(peak, std::abs(int(v)));
        check(peak > 1000, std::string(c.file) + ": not silence");
    }
    {
        std::string why;
        const std::vector<uint8_t> junk{'n', 'o', 't', ' ', 'a', 'u', 'd', 'i', 'o'};
        check(!ta::music::decode(junk, 44100, &why) && !why.empty(), "a file that is not audio is refused, with a reason");
        check(!ta::music::decode({}, 44100, &why), "an empty file is refused");
    }
    ta::music::setCustom({{15, "/m/menu.mp3"}});
    check(ta::music::customFile(15) == "/m/menu.mp3" && ta::music::customFile(3).empty(), "a chosen track plays its file, the rest the originals");
    ta::music::setCustom({});
    check(ta::music::customFile(15).empty(), "cleared: the original");
    {
        // A game plays the chosen file for track 15; the title screen and the lobby
        // (loadTrack with custom off) keep the original. Original: tone.wav (mono),
        // chosen: tone.ogg (stereo).
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / ("ta-music-test-" + std::to_string(::getpid()));
        fs::create_directories(root / "Music");
        fs::copy_file(dir + "tone.wav", root / "Music" / "track15.wav", fs::copy_options::overwrite_existing);
        ta::hpi::Vfs vfs;
        vfs.addLayer(ta::hpi::MountSet(root / "Music",
                         ta::hpi::MountConfig{.includeLoose = true, .archiveExts = {}, .keep = {}, .archiveNames = {}}),
                     "music/");
        ta::music::setCustom({{15, dir + "tone.ogg"}});
        const auto game = ta::music::loadTrack(vfs, 15, 44100);
        const auto menu = ta::music::loadTrack(vfs, 15, 44100, false);
        check(game && game->channels == 2, "in games track 15 plays the chosen file");
        check(menu && menu->channels == 1, "the title screen and lobby play the original");
        ta::music::setCustom({});
        fs::remove_all(root);
    }
    // Optional: time a long file (tools/music_decode_test <file>).
    if (argc > 1) {
        const auto bytes = slurp(argv[1]);
        const auto t0 = std::chrono::steady_clock::now();
        const auto pcm = ta::music::decode(bytes, 44100);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  %s: %s, %.0f ms\n", argv[1], pcm ? "decoded" : "FAILED", ms);
    }
    std::printf(g_fail ? "music_decode_test: %d FAILURE(S)\n" : "music_decode_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
