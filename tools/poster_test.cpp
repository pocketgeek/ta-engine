// MakePoster's pieces that run without a window: the MAKE POSTER hotkey (Print
// Screen by default, rebindable like every action) and the streaming RGB PNG
// writer the poster is saved with (decoded back here byte for byte).
#include "client/hotkeys.h"
#include "util/png.h"

#include <SDL.h>
#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace tak;

static void check(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// A minimal PNG reader for what writeRgb produces: 8-bit RGB, filters 0/1.
static std::vector<uint8_t> readRgb(const std::filesystem::path& file, int& w, int& h) {
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
    check(b.size() > 8 && std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) == 0, "PNG signature");
    std::vector<uint8_t> z;
    size_t at = 8;
    bool end = false;
    while (at + 12 <= b.size()) {
        const uint32_t n = be32(&b[at]);
        const std::string type(reinterpret_cast<const char*>(&b[at + 4]), 4);
        check(at + 12 + n <= b.size(), "chunk inside the file");
        const uLong crc = crc32(crc32(0, &b[at + 4], 4), &b[at + 8], uInt(n));
        check(crc == be32(&b[at + 8 + n]), "chunk CRC");
        if (type == "IHDR") {
            w = int(be32(&b[at + 8])); h = int(be32(&b[at + 12]));
            check(b[at + 16] == 8 && b[at + 17] == 2, "8-bit RGB");
        } else if (type == "IDAT") {
            z.insert(z.end(), b.begin() + std::ptrdiff_t(at + 8), b.begin() + std::ptrdiff_t(at + 8 + n));
        } else if (type == "IEND") { end = true; }
        at += 12 + n;
    }
    check(end && at == b.size(), "IEND last");
    std::vector<uint8_t> raw(size_t(w) * h * 3 + size_t(h));
    uLongf len = uLongf(raw.size());
    check(uncompress(raw.data(), &len, z.data(), uLong(z.size())) == Z_OK && len == raw.size(), "inflate");
    std::vector<uint8_t> out(size_t(w) * h * 3);
    const size_t stride = size_t(w) * 3;
    for (int y = 0; y < h; ++y) {
        const uint8_t filter = raw[size_t(y) * (stride + 1)];
        check(filter <= 1, "filter");
        const uint8_t* src = &raw[size_t(y) * (stride + 1) + 1];
        uint8_t* dst = &out[size_t(y) * stride];
        for (size_t i = 0; i < stride; ++i)
            dst[i] = uint8_t(src[i] + (filter == 1 && i >= 3 ? dst[i - 3] : 0));
    }
    return out;
}

int main(int, char**) {
    // The hotkey.
    Hotkeys hk;
    hk.load({});
    check(hk.chord(Act::MakePoster) == KeyChord{SDLK_p, HK_SHIFT}, "MAKE POSTER defaults to Shift+P");
    check(hk.match(SDLK_p, KMOD_LSHIFT) == Act::MakePoster, "Shift+P makes a poster");
    check(hk.match(SDLK_p, 0) == Act::Patrol, "P alone is still Patrol");
    check(hk.match(SDLK_p, KMOD_LSHIFT | KMOD_LCTRL) != Act::MakePoster, "the chord is exact");
    int shiftPUsers = 0;
    for (const auto& d : hotkeyDefs()) shiftPUsers += d.def == KeyChord{SDLK_p, HK_SHIFT};
    check(shiftPUsers == 1, "no other default action takes Shift+P");
    std::map<std::string, std::string> overrides;
    setChordOverride(overrides, Act::MakePoster, KeyChord{SDLK_F9, HK_CTRL});
    check(overrides.count("view.poster") == 1, "a rebind persists under view.poster");
    hk.load(overrides);
    check(hk.match(SDLK_F9, KMOD_LCTRL) == Act::MakePoster && hk.match(SDLK_p, KMOD_LSHIFT) != Act::MakePoster,
          "the rebound key makes the poster, Shift+P no longer does");
    {   // The streaming panel's key, read by the panel itself.
        std::map<std::string, std::string> keys;
        check(streamingPanelPress(keys, SDLK_F9, 0, false) && streamingPanelPress(keys, SDLK_F9, 0, true),
              "F9 opens the streaming panel by default, typing or not");
        check(!streamingPanelPress(keys, SDLK_F9, KMOD_LCTRL, false), "Ctrl+F9 is not the panel's");
        setChordOverride(keys, Act::StreamingPanel, KeyChord{SDLK_p, 0});
        check(streamingPanelPress(keys, SDLK_p, 0, false) && !streamingPanelPress(keys, SDLK_F9, 0, false),
              "a rebound panel key replaces F9");
        check(!streamingPanelPress(keys, SDLK_p, 0, true), "a plain letter types while a text field has the keyboard");
        setChordOverride(keys, Act::StreamingPanel, KeyChord{SDLK_p, HK_CTRL});
        check(streamingPanelPress(keys, SDLK_p, KMOD_LCTRL, true), "Ctrl+letter still works while typing");
        setChordOverride(keys, Act::StreamingPanel, KeyChord{});
        check(!streamingPanelPress(keys, SDLK_F9, 0, false), "an unbound panel has no key");
    }
    setChordOverride(overrides, Act::MakePoster, KeyChord{});
    hk.load(overrides);
    check(!hk.chord(Act::MakePoster).bound() && overrides["view.poster"] == "NONE", "MAKE POSTER can be unbound");
    setChordOverride(overrides, Act::MakePoster, KeyChord{SDLK_p, HK_SHIFT});
    check(!overrides.count("view.poster"), "back to the default drops the override");

    // The writer: an odd size, every byte checked after a decode.
    const int w = 333, h = 77;
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    uint32_t seed = 12345;
    for (size_t i = 0; i < rgb.size(); ++i) {
        seed = seed * 1103515245u + 12345u;
        rgb[i] = (i / 3) % 7 == 0 ? uint8_t(seed >> 16) : uint8_t(i / 999);   // noise and flat runs
    }
    const auto file = std::filesystem::temp_directory_path() / ("tak-poster-test-" + std::to_string(SDL_GetTicks64()) + ".png");
    png::writeRgb(file, w, h, rgb);
    int rw = 0, rh = 0;
    const auto back = readRgb(file, rw, rh);
    std::filesystem::remove(file);
    check(rw == w && rh == h, "the poster has the map's size");
    check(back == rgb, "every pixel survives the PNG");
    bool threw = false;
    try { png::writeRgb(file, w, h, std::vector<uint8_t>(10)); } catch (const std::exception&) { threw = true; }
    check(threw, "a wrong-sized buffer is refused");
    std::puts("PASS: MAKE POSTER hotkey (Shift+P, rebindable) and the streaming RGB PNG writer");
    return 0;
}
