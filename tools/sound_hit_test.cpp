// Weapon impact sounds resolve to real WAVs.
//
// A sound-class table lists its impact classes as "soundN = FILE.wav", while unit
// voice classes are weighted "NAME = weight" entries. Reading the impact classes the
// voice way picked a sound called "sound0" that does not exist, so every hit was
// silent. Without data this checks the layout helpers. TA has no class tables: a
// weapon's soundhit= (weapons/*.tdf) names the WAV itself, and the client plays it
// directly (gameview_impl's impact loop). With a retail install every soundhit a
// shipped unit's weapon names must be in the sound bank.
#include "client/sound.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <iostream>
#include <set>

// SoundBank's output path is linked in but never opened here: index-only use.
namespace ta {
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
        auto vfs = ta::hpi::mountRetailRoot(argv[1]);
        SoundBank bank; bank.reload(vfs);
        ta::sim::TypeRegistry registry; ta::sim::setupRegistry(registry, vfs);
        std::set<std::string> hits;
        for (const auto& [id, type] : registry.types())
            for (const auto& weapon : type.weapons)
                if (!weapon.soundHit.empty()) hits.insert(weapon.soundHit);
        for (const auto& wav : hits) check(bank.has(wav), "impact sound " + wav + " is in the bank");
        check(hits.size() >= 10, "shipped weapons name their impact sounds (" + std::to_string(hits.size()) + ")");
        // The commonest soundhit in weapons/*.tdf.
        check(hits.count("xplomed2") && bank.has("xplomed2"), "xplomed2 resolves");
    }
    if (g_fail) { std::cerr << g_fail << " failure(s)\n"; return 1; }
    std::cout << "PASS: sound hit classes\n";
    return 0;
}
