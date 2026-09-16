// tdf_test -- the TDF text-config parser (.fbi / .tdf / .ota / .gui).
//
// Data-independent, so CI runs it. It exists because retargeting the parser at
// TA data changed a load-bearing rule and the regression it caused was SILENT:
// one unit stopped loading and the only symptom was a roster count one lower
// than before.
//
// The rule that changed: a value used to run to END OF LINE, inherited from the
// Kingdoms engine whose data had values legitimately containing ';'. TA needs
// ';' to terminate, because SIDEDATA.TDF packs all four keys of a panel rect
// onto one line 240 times over -- read to end of line, every rect came back as
// its x1 and three zeros, and the HUD layout was unreadable.

#include "tdf/tdf.h"
#include "tdf/sidedata.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("  %-68s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}
static void eq(const std::string& got, const std::string& want, const std::string& what) {
    bool ok = got == want;
    std::printf("  %-68s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) { std::printf("      got '%s', want '%s'\n", got.c_str(), want.c_str()); ++g_fail; }
}

int main() {
    std::printf("tdf_test\n");

    // --- several assignments on one line ------------------------------------
    // The shape SIDEDATA states every panel rect in.
    {
        auto n = ta::tdf::parseText("[R]\n{ x1=132; y1=5; x2=152; y2=25; }\n");
        const auto* r = n.child("R");
        check(r != nullptr, "a one-line section parses");
        if (r) {
            eq(std::to_string(int(r->numberOr("x1", -1))), "132", "x1 reads");
            eq(std::to_string(int(r->numberOr("y1", -1))), "5", "y1 reads (not swallowed by x1)");
            eq(std::to_string(int(r->numberOr("x2", -1))), "152", "x2 reads");
            eq(std::to_string(int(r->numberOr("y2", -1))), "25", "y2 reads");
        }
    }

    // --- a bare token with no '=' is skipped, not fatal ----------------------
    // ARMSCORP.FBI ships "ItalianDescription=;Scorpione". Once ';' terminates,
    // "Scorpione" is a stray token; failing on it abandoned the whole file and
    // dropped the Core Contingency Scorpion from the roster with no error.
    {
        auto n = ta::tdf::parseText(
            "[UNITINFO]\n{\nUnitName=ARMSCORP;\nItalianDescription=;Scorpione\nSide=ARM;\n}\n");
        const auto* u = n.child("UNITINFO");
        check(u != nullptr, "a file with a stray token still parses");
        if (u) {
            eq(u->valueOr("unitname", ""), "ARMSCORP", "keys before the stray token survive");
            eq(u->valueOr("italiandescription", "?"), "", "the malformed value reads empty");
            eq(u->valueOr("side", ""), "ARM", "and keys AFTER it still parse");
        }
    }

    // --- comments end a value, and do not become part of it ------------------
    // WEAPONS.TDF writes "rendertype=4;	/* 2D bitmap */".
    {
        auto n = ta::tdf::parseText(
            "[W]\n{\nrendertype=4;\t/* 2D bitmap */\nname=hi;  // trailing\ncolor=232;\n}\n");
        const auto* w = n.child("W");
        check(w != nullptr, "a section with comments parses");
        if (w) {
            eq(w->valueOr("rendertype", ""), "4", "a /* */ comment does not join the value");
            eq(w->valueOr("name", ""), "hi", "nor does a // comment");
            eq(w->valueOr("color", ""), "232", "and parsing continues past both");
        }
    }

    // --- nesting, ordering, case ---------------------------------------------
    {
        auto n = ta::tdf::parseText(
            "[Outer]\n{\nKey=Value;\n[Inner]\n{\na=1;\n}\n[Second]\n{\nb=2;\n}\n}\n");
        const auto* o = n.child("outer");
        check(o != nullptr, "section lookup is case-insensitive");
        if (o) {
            eq(o->valueOr("KEY", ""), "Value", "key lookup is case-insensitive");
            check(o->child("inner") && o->child("second"), "both children parse");
            check(o->childOrder.size() == 2 && o->childOrder[0] == "inner",
                  "childOrder preserves declaration order");
        }
    }

    // --- SideData over a miniature SIDEDATA ----------------------------------
    {
        auto sd = ta::tdf::SideData::parse(
            "[SIDE0]\n{\nname=ARM;\nnameprefix=ARM;\ncommander=ARMCOM;\n"
            "[METALBAR]\n{ x1=218; y1=11; x2=339; y2=13; }\n}\n"
            "[SIDE1]\n{\nname=CORE;\nnameprefix=COR;\ncommander=CORCOM;\n}\n"
            "[CANBUILD]\n{\n[ARMCOM]\n{\ncanbuild1=ARMSOLAR;\ncanbuild2=ARMWIN;\n"
            "canbuild10=ARMSY;\ncanbuild3=ARMMEX;\n}\n}\n");
        check(sd.sides.size() == 2, "both sides parse");
        check(sd.side("arm") != nullptr, "side lookup is case-insensitive");
        check(sd.sideByPrefix("COR") && sd.sideByPrefix("COR")->name == "CORE",
              "CORE is found by its COR unit prefix, which differs from its name");
        const auto* s0 = sd.side("ARM");
        const auto* mb = s0 ? s0->panel("metalbar") : nullptr;
        check(mb && mb->x1 == 218 && mb->y2 == 13, "a panel rect survives into SideData");
        check(mb && mb->width() == 121 && mb->height() == 2, "width/height derive correctly");

        // canbuild10 is declared THIRD but numbered tenth. The menu order is the
        // numbering, and the walk stops at the first gap -- so a canbuild10 with
        // no canbuild4 must not appear, and must certainly not sort next to 1.
        auto it = sd.canBuild.find("armcom");
        check(it != sd.canBuild.end(), "the build menu parses");
        if (it != sd.canBuild.end()) {
            const auto& m = it->second;
            check(m.size() == 3, "the walk stops at the first missing index");
            check(m.size() == 3 && m[0] == "armsolar" && m[1] == "armwin" &&
                  m[2] == "armmex",
                  "and yields canbuild1,2,3 in order (not the declaration order)");
        }
    }

    // --- an empty/absent install must not throw ------------------------------
    {
        auto sd = ta::tdf::SideData::parse("");
        check(sd.sides.empty() && sd.canBuild.empty(),
              "empty input yields an empty SideData rather than throwing");
    }

    std::printf(g_fail ? "tdf_test: %d FAILURE(S)\n" : "tdf_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
