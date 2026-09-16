// aiprofile_test -- TA's ai/*.txt build profiles, and the .ota start positions.
//
// Data-independent, so CI runs it. Both parsers are here because they caused the
// SAME failure from two directions: a headless skirmish in which neither AI ever
// built anything, with no error printed by anything.
//
// The rules worth pinning, each of which was wrong and silent:
//
//   * A unit the profile does not mention has weight 1.0, not 0. TA's format
//     states ADJUSTMENTS to a flat default; DEFAULT.TXT names ~90 of ~270 units,
//     so reading "absent" as "never build" paralyses the AI completely.
//   * Weights are FRACTIONAL ("0.2", ".1", "1.25"). Parsed as int they truncate
//     to 0 -- which is the paralysing case above, arrived at a second way.
//   * A Weight/Limit key may be a unit id OR an FBI Category tag (CONSTR, PLANT,
//     LEVEL3, ARM...). Matching ids only drops every one of the file's broad
//     strokes, which is most of what it says.
//   * .ota start positions are nested [GlobalHeader][Schema N][specials], not
//     under a "map data" section, and XPos/ZPos are WORLD units, not cells.
//     Scaling by 16 threw every start off the map; setupMatch then fell back to
//     the map centre, which on a coastal map is open ocean -- so no building
//     could be sited anywhere and the AI looked like it had an empty menu.

#include "ai/ai.h"
#include "sim/matchsetup.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("  %-70s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}
static void eqi(int got, int want, const std::string& what) {
    bool ok = got == want;
    std::printf("  %-70s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) { std::printf("      got %d, want %d\n", got, want); ++g_fail; }
}

// A unit type carrying just what the profile lookup reads.
static ta::sim::UnitType unit(const std::string& id,
                              const std::vector<std::string>& cats) {
    ta::sim::UnitType t;
    t.id = id;
    t.categories = cats;
    return t;
}

int main() {
    std::printf("aiprofile_test\n");

    // --- the shape of a real DEFAULT.TXT -------------------------------------
    {
        auto p = ta::ai::parseProfile(
            "// DEFAULT PROFILE\n"
            "weight cormakr 0.25\n"        // global: applies to every plan
            "limit special 10\n"
            "\n"
            "plan easy\n"
            "Weight ARM 0.2\n"
            "Weight CONSTR 3\n"
            "Limit constr 3\n"
            "Weight ARMMAKR .1\n"
            "\n"
            "plan medium\n"
            "Weight CONSTR 4\n"
            "Limit constr 5\n"
            "\n"
            "plan hard\n"
            "Weight CONSTR 4\n"
            "Limit constr 6\n");

        const auto& easy = p.forDifficulty(ta::ai::Difficulty::Easy);
        const auto& med  = p.forDifficulty(ta::ai::Difficulty::Normal);
        const auto& hard = p.forDifficulty(ta::ai::Difficulty::Hard);

        // The single most important rule: silence means 1.0.
        const auto plain = unit("armsolar", {"arm", "energy", "level1"});
        eqi(ta::ai::profileWeight(med, plain), ta::ai::kWeightOne,
            "a unit the profile never mentions keeps the default weight");
        eqi(ta::ai::profileLimit(med, plain), -1,
            "and no limit");

        // Fractional weights survive; ".1" and "0.25" are both real syntax.
        eqi(easy.weight.at("armmakr"), 10,  "'.1' parses to 0.10");
        eqi(easy.weight.at("cormakr"), 25,  "'0.25' parses to 0.25");
        eqi(easy.weight.at("arm"), 20,      "'0.2' parses to 0.20");

        // Lines before the first `plan` reach every plan.
        check(easy.weight.count("cormakr") && med.weight.count("cormakr") &&
              hard.weight.count("cormakr"),
              "a weight stated before the first 'plan' applies to all three plans");
        eqi(easy.limit.at("special"), 10, "so does a limit (and a limit is a whole number)");

        // ...and a plan section is scoped to itself alone.
        check(easy.weight.count("arm") && !med.weight.count("arm"),
              "a weight inside 'plan easy' does NOT leak into 'plan medium'");

        // Category keys resolve through the unit's FBI Category tags.
        const auto ck = unit("armck", {"arm", "constr", "level1"});
        // arm 0.2 * constr 3 = 0.6
        eqi(ta::ai::profileWeight(easy, ck), 60,
            "categories compound: arm(0.2) x constr(3) = 0.60");
        eqi(ta::ai::profileLimit(easy, ck), 3, "and a category limit applies to the unit");
        eqi(ta::ai::profileLimit(med, ck), 5,  "limits ramp with the plan");

        // An explicit per-unit entry is the author being specific: it wins outright
        // rather than compounding with the unit's categories.
        const auto makr = unit("armmakr", {"arm", "energy"});
        eqi(ta::ai::profileWeight(easy, makr), 10,
            "a unit-id weight overrides its categories rather than multiplying with them");

        // Difficulty -> plan mapping, including the two that share a sibling's plan.
        eqi(p.forDifficulty(ta::ai::Difficulty::Passive).weight.count("arm") ? 1 : 0, 1,
            "Passive shares Easy's plan");
        eqi(p.forDifficulty(ta::ai::Difficulty::Absurd).limit.at("constr"), 6,
            "Absurd shares Hard's plan");
    }

    // --- malformed lines retail actually ships -------------------------------
    // MISSIONS.TXT contains "Limit ARMSABO" with no value at all. Aborting the
    // file there would discard every rule after it.
    {
        auto p = ta::ai::parseProfile(
            "plan medium\n"
            "limit corint 0\n"
            "Limit ARMSABO\n"           // no value
            "limit cordoom 0\n");
        const auto& med = p.forDifficulty(ta::ai::Difficulty::Normal);
        eqi(med.limit.at("corint"), 0, "a limit before the malformed line survives");
        check(!med.limit.count("armsabo"), "the valueless line is skipped");
        eqi(med.limit.at("cordoom"), 0, "and parsing CONTINUES past it");

        // `limit x 0` means never build x -- distinct from "no limit stated".
        const auto intim = unit("corint", {"core", "weapon"});
        eqi(ta::ai::profileLimit(med, intim), 0, "an explicit limit of 0 is preserved, not read as unlimited");
    }

    // --- an absent/empty profile is playable, not paralysed ------------------
    {
        auto p = ta::ai::parseProfile("");
        const auto& med = p.forDifficulty(ta::ai::Difficulty::Normal);
        eqi(ta::ai::profileWeight(med, unit("armpw", {"arm", "kbot"})), ta::ai::kWeightOne,
            "an empty profile still yields buildable units");
    }

    // --- .ota start positions -------------------------------------------------
    {
        // The real nesting, with the specials deliberately out of StartPos order
        // (Coast To Coast's file opens with StartPos3) to pin the reordering.
        const std::string ota =
            "[GlobalHeader]\n{\n"
            "missionname=Coast To Coast;\n"
            "  [Schema 0]\n  {\n"
            "  Type=Network 1;\n"
            "  SurfaceMetal=5;\n"
            "    [specials]\n    {\n"
            "      [special0]\n      { specialwhat=StartPos3; XPos=3010; ZPos=1497; }\n"
            "      [special1]\n      { specialwhat=StartPos1; XPos=134; ZPos=236; }\n"
            "      [special2]\n      { specialwhat=StartPos2; XPos=3236; ZPos=247; }\n"
            "    }\n  }\n}\n";
        auto sp = ta::sim::parseStartPositionsText(ota);
        check(sp.size() == 3, "all three start positions parse from under [Schema 0]");
        if (sp.size() == 3) {
            // Ordered by the StartPos NUMBER, not by declaration order.
            eqi(int(sp[0].first), 134,  "StartPos1 sorts first despite being declared second");
            eqi(int(sp[1].first), 3236, "StartPos2 second");
            eqi(int(sp[2].first), 3010, "StartPos3 third");
            // World units, NOT cells: a x16 scaling would give 2144 here.
            eqi(int(sp[0].second), 236, "ZPos is taken in world units, unscaled");
            // Coast To Coast is 210x126 cells = 3360x2016 world units. Every
            // position must land inside that, which x16 scaling never could.
            bool inside = true;
            for (auto& [x, z] : sp) if (x < 0 || x > 3360 || z < 0 || z > 2016) inside = false;
            check(inside, "every start position lands inside the map's world bounds");
        }

        // A file with no specials yields nothing rather than throwing.
        check(ta::sim::parseStartPositionsText("[GlobalHeader]\n{\n}\n").empty(),
              "an .ota with no schema yields no start positions rather than throwing");
        check(ta::sim::parseStartPositionsText("").empty(), "so does empty input");
    }

    std::printf(g_fail ? "aiprofile_test: %d FAILURE(S)\n" : "aiprofile_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
