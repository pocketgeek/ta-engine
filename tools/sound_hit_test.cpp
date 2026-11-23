// Weapon impact sounds (soundhitclass) resolve to real WAVs.
//
// soundclasses.tdf lists its impact classes as "soundN = FILE.wav", while unit voice
// classes are weighted "NAME = weight" entries. Reading the impact classes the voice
// way picked a sound called "sound0" that does not exist, so every sword, arrow,
// catapult and cannon hit was silent. Without data this checks the layout helpers;
// with a retail install it resolves every class/material the shipped weapons use.
#include "client/sound.h"
#include "hpi/hpi.h"
#include <iostream>
#include <set>

// SoundBank's output path is linked in but never opened here: index-only use.
namespace tak {
int detectOutputChannels() { return 2; }
SDL_AudioDeviceID openAudioDevice(int capture, const SDL_AudioSpec* want,
                                  SDL_AudioSpec* have, int allowed) {
    return SDL_OpenAudioDevice(nullptr, capture, want, have, allowed);
}
}

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_fail; }
}

int main(int argc, char** argv) {
    check(SoundClasses::isVariantKey("sound0") && SoundClasses::isVariantKey("Sound12"),
          "soundN keys are variants");
    check(!SoundClasses::isVariantKey("TONEARA") && !SoundClasses::isVariantKey("sound") &&
          !SoundClasses::isVariantKey("soundx1"), "voice names are not variants");
    check(SoundClasses::wavStem("RHITFLS1.wav") == "rhitfls1", "WAV value -> bank stem");
    if (argc == 2) {
        auto vfs = tak::hpi::mountRetailRoot(argv[1]);
        SoundBank bank; bank.reload(vfs);
        SoundClasses classes; classes.load(vfs);
        // Every impact class a shipped weapon names, with each body material.
        const std::set<std::string> impact = {"sword", "arrow", "fire", "lightning", "rock",
                                              "fist", "cannon", "hammer"};
        for (const auto& cls : impact)
            for (const char* body : {"default", "flesh", "armor", "wood", "stone"}) {
                const std::string* wav = classes.pick(cls, body, 7);
                if (!wav && std::string(body) != "default") wav = classes.pick(cls, "default", 7);
                check(wav && bank.has(*wav), "impact " + cls + "/" + body + " -> " +
                      (wav ? *wav : std::string("(none)")));
            }
        // The Aramon catapult's ground hit, the case that was reported.
        const std::string* rock = classes.pick("rock", "default", 0);
        check(rock && *rock == "rhitfls1" && bank.has(*rock), "catapult ground hit is rhitfls1");
        // Voice classes keep their weighted layout.
        const std::string* move = classes.pick("arapult", "move", 0);
        check(move && bank.has(*move), "unit voice class still resolves");
    }
    if (g_fail) { std::cerr << g_fail << " failure(s)\n"; return 1; }
    std::cout << "PASS: sound hit classes\n";
    return 0;
}
