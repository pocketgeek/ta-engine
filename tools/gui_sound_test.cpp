// TA's GUI sounds (client/guisound.h): the allsound.tdf entry each pressed gadget
// requests (TotalA.exe's per-handler 0x47f1a0 calls), the table's lookup rules, and,
// with an install, that every entry the GUI uses names a shipped WAV that decodes.
#include "client/guisound.h"
#include "client/menusfx.h"
#include "hpi/hpi.h"

#include <SDL.h>

#include <cstdio>
#include <stdexcept>
#include <string>

static void check(bool yes, const std::string& message) {
    if (!yes) throw std::runtime_error(message);
}

int main(int argc, char** argv) try {
    namespace gs = ta::guisound;
    // The title screen (0x425d80) and the screens it opens.
    check(gs::frontEnd("MAINMENU", "SINGLE") == "BIGBUTTON", "SINGLE: BigButton (0x425dbd)");
    check(gs::frontEnd("mainmenu", "Multi") == "BIGBUTTON", "MULTI: BigButton, any case (0x425dfa)");
    check(gs::frontEnd("MAINMENU", "INTRO") == "SMLBUTTON", "INTRO: smlButton (0x425ee4)");
    check(gs::frontEnd("MAINMENU", "EXIT") == "EXIT", "EXIT: exit (0x426076)");
    check(gs::frontEnd("MAINMENU", "OPTIONS") == "OPTIONS", "the engine's OPTIONS plays Options");
    check(gs::frontEnd("MAINMENU", "CAMPAIGN") == "BIGBUTTON", "the engine's CAMPAIGN plays BigButton");
    check(gs::frontEnd("SINGLE", "Skirmish") == "SKIRMISH", "SINGLE Skirmish: skirmish (0x477622)");
    check(gs::frontEnd("SINGLE", "PrevMenu") == "PREVIOUS", "PrevMenu: Previous (0x4776f2)");
    check(gs::frontEnd("NEWGAME", "Core") == "SIDESELECT2" && gs::frontEnd("NEWGAME", "Side0") == "SIDESELECT",
          "the side buttons (0x477def / 0x47802a)");
    check(gs::frontEnd("NEWGAME", "Difficulty") == "SMLBUTTON", "NEWGAME Difficulty: SmlButton (0x477d0b)");
    check(gs::frontEnd("MSNBRIEF", "MOREBAR") == "MORE", "MSNBRIEF MOREBAR: More (0x478e5f)");
    check(gs::frontEnd("SKIRMISH", "LineOfSight") == "SKIRMISH" && gs::frontEnd("SKIRMISH", "Start") == "BIGBUTTON",
          "SKIRMISH settings and Start (0x47b7ec / 0x47aee1)");
    check(gs::frontEnd("SELMAP", "LOAD") == "SMALLBUTTON" && gs::frontEnd("SELMAP", "MAPNAMES").empty(),
          "SELMAP LOAD: SmallButton, its list none (0x47a9f0)");
    check(gs::frontEnd("ENDMSN", "Difficulty") == "SKIRMISH", "ENDMSN Difficulty: SKirmish (0x41ee8b)");
    check(gs::frontEnd("MAINMENU", "DebugString").empty() && gs::frontEnd("NOSUCH", "SINGLE").empty(),
          "no sound for gadgets and screens retail plays none for");
    // The in-game sidebar (0x419be0 / 0x41aa60).
    check(gs::sidebar("MOVE") == "IMMEDIATEORDERS" && gs::sidebar("STOP") == "IMMEDIATEORDERS",
          "MOVE / STOP: immediateorders");
    check(gs::sidebar("RECLAIM") == "SPECIALORDERS" && gs::sidebar("CLOAK") == "SPECIALORDERS",
          "RECLAIM / CLOAK: specialorders");
    check(gs::sidebar("FIREORD") == "SETFIREORDERS" && gs::sidebar("MOVEORD") == "SETMOVEORDERS", "the stance buttons");
    check(gs::sidebar("BUILD") == "BUILDBUTTON" && gs::sidebar("ORDERS") == "ORDERSBUTTON", "the tabs");
    check(gs::sidebar("NEXT") == "NEXTBUILDMENU", "the build pages");
    check(gs::sidebar("ARMSOLAR", true) == "ADDBUILD" && gs::sidebar("ARMSOLAR", true, true) == "SUBBUILD",
          "a build picture adds (left) / removes (right) (0x419b00)");
    // The engine's own menus.
    check(gs::lobby("START") == "BIGBUTTON" && gs::lobby("Cancel") == "PREVIOUS" && gs::lobby("KICK") == "MULTI",
          "the engine lobby's buttons");
    check(gs::engineClick("OK.WAV") == "BIGBUTTON" && gs::engineClick("cancel.wav") == "PREVIOUS" &&
              gs::engineClick("butmain1.wav").empty(),
          "the Kingdoms click names map to retail entries");

    // The table: first section wins, case-insensitive, ".wav" added to a bare stem.
    gs::Table t;
    t.parse("[BIGBUTTON]\n{\n\tsound=butmain1;\n}\n[bigbutton]\n{\n\tsound=other;\n}\n"
            "[MAINMENU]\n{\n\tsound=explode.wav;\n}\n");
    check(t.wav("BigButton") == "butmain1.wav", "first BIGBUTTON wins, any case: " + t.wav("BigButton"));
    check(t.wav("MAINMENU") == "explode.wav", "an entry with .wav keeps it");
    check(t.wav("NOSUCH").empty(), "an unknown entry plays nothing (0x47f1ef)");

    if (argc > 1) {
        // The install: every entry the GUI names resolves to a WAV that is shipped and decodes.
        auto vfs = ta::hpi::mountRetailRoot(argv[1]);
        gs::Table real;
        real.load(vfs);
        check(real.wav("BIGBUTTON") == "butmain1.wav" && real.wav("EXIT") == "button2.wav" &&
                  real.wav("IMMEDIATEORDERS") == "button5.wav",
              "the shipped allsound.tdf");
        const char* used[] = {"BIGBUTTON", "SMLBUTTON", "SMALLBUTTON", "SKIRMISH", "MULTI", "OPTIONS", "EXIT",
                              "PREVIOUS", "SIDESELECT", "SIDESELECT2", "MORE", "ALLY", "ORDERSBUTTON", "BUILDBUTTON",
                              "ADDBUILD", "SUBBUILD", "NEXTBUILDMENU", "IMMEDIATEORDERS", "SPECIALORDERS",
                              "SETFIREORDERS", "SETMOVEORDERS", "OKTOBUILD", "NOTOKTOBUILD"};
        // The engine menus' clicks (MenuSfx): an entry through the table, a shipped WAV
        // as is, and the Kingdoms click names TA lacks as their retail entries.
        ta::MenuSfx sfx(vfs);
        check(sfx.resolve("BIGBUTTON") == "butmain1.wav", "MenuSfx plays an allsound entry");
        check(sfx.resolve("butmain4.wav") == "butmain4.wav", "MenuSfx plays a shipped WAV as is");
        check(sfx.resolve("ok.wav") == "butmain1.wav" && sfx.resolve("cancel.wav") == "button1.wav" &&
                  sfx.resolve("menubutton.wav") == "butoptn.wav",
              "the engine menus' ok / cancel / page clicks play BigButton / Previous / Options");
        for (const char* e : used) {
            const std::string wav = real.wav(e);
            check(!wav.empty(), std::string("allsound.tdf has [") + e + "]");
            const auto bytes = vfs.read("sounds/" + wav);
            SDL_AudioSpec spec{};
            Uint8* buf = nullptr;
            Uint32 len = 0;
            check(SDL_LoadWAV_RW(SDL_RWFromConstMem(bytes.data(), int(bytes.size())), 1, &spec, &buf, &len) && len > 0,
                  "sounds/" + wav + " decodes");
            SDL_FreeWAV(buf);
            std::printf("%-16s %-13s %d Hz %d ch %u bytes\n", e, wav.c_str(), spec.freq, int(spec.channels), len);
        }
    }
    std::puts("gui sound ok");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
