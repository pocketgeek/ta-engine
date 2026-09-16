// campaign_test -- the campaign spine, camps/<name>.tdf.
//
// Data-independent, so CI runs it.
//
// The rule worth pinning is which key gives the mission's FILE. Both games list
// `missionfile` and `missionname` in each [MISSIONn], and they do not mean the
// same thing:
//
//   TA        missionfile=AC01.ota;  missionname=1: A Hero Returns;
//   Kingdoms  missionfile=takmission01_mt.ota;  missionname=takmission01_mt;
//
// Kingdoms repeats the stem in `missionname`, so reading the stem from there
// worked -- and silently asked a TA install for a file called "1: A Hero
// Returns", which is not a filename. Every TA campaign then resolved to nothing.
// The stem comes from `missionfile`; `missionname` is a display title in TA and
// is left to translate/missions.tdf in Kingdoms.

#include "campaign/campaign.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("  %-70s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}
static void eqs(const std::string& got, const std::string& want, const std::string& what) {
    bool ok = got == want;
    std::printf("  %-70s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) { std::printf("      got '%s', want '%s'\n", got.c_str(), want.c_str()); ++g_fail; }
}
static void eqi(int got, int want, const std::string& what) {
    bool ok = got == want;
    std::printf("  %-70s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) { std::printf("      got %d, want %d\n", got, want); ++g_fail; }
}

int main() {
    std::printf("campaign_test\n");

    // --- TA: missionname is a DISPLAY TITLE -----------------------------------
    {
        ta::Campaign c;
        const bool ok = ta::parseCampaignText(
            "[HEADER]\n\t{\n\tcampaignside=ARM;\n\t}\n\n"
            "[MISSION0]\n\t{\n\tmissionfile=AC01.ota;\n"
            "\tmissionname=1: A Hero Returns;\n"
            "\tGermanmissionname=1: Ein Held kehrt zurueck;\n\t}\n"
            "[MISSION1]\n\t{\n\tmissionfile=AC02.ota;\n"
            "\tmissionname=2: CORE KBot Base, Destroy It!;\n\t}\n",
            "camps/Arm Campaign.tdf", c);
        check(ok, "a TA campaign parses");
        eqs(c.side, "ARM", "the campaign side");
        eqi(c.count(), 2, "both missions");
        if (c.count() == 2) {
            eqs(c.missions[0].stem, "ac01",
                "the stem comes from missionfile, NOT the display name");
            eqs(c.missions[0].otaFile, "AC01.ota", "and the .ota file is kept");
            eqs(c.missions[0].title, "1: A Hero Returns",
                "missionname becomes the TITLE");
            eqs(c.missions[1].stem, "ac02", "...and the same for the second");
            // The point of the whole thing: the stem has to be usable as a path.
            check(c.missions[0].stem.find(':') == std::string::npos &&
                  c.missions[0].stem.find(' ') == std::string::npos,
                  "the stem is a filename, not a sentence");
        }
    }

    // --- Kingdoms: missionname repeats the stem -------------------------------
    {
        ta::Campaign c;
        ta::parseCampaignText(
            "[HEADER]\n{\ncampaignside=aramon;\n}\n"
            "[MISSION0]\n{\nmissionfile=takmission01_mt.ota;\n"
            "missionname=takmission01_mt;\n}\n",
            "camps/Book Of Darien.tdf", c);
        eqi(c.count(), 1, "a Kingdoms campaign still parses");
        if (c.count() == 1) {
            eqs(c.missions[0].stem, "takmission01_mt", "its stem is unchanged");
            // When the two agree there is no title to take from the file: Kingdoms
            // supplies it from translate/missions.tdf afterwards, and leaving it
            // empty is what tells the caller to do that.
            eqs(c.missions[0].title, "", "and no inline title is invented for it");
        }
    }

    // --- a missing missionfile falls back to missionname ----------------------
    {
        ta::Campaign c;
        ta::parseCampaignText(
            "[MISSION0]\n{\nmissionname=somemission;\n}\n", "camps/x.tdf", c);
        eqi(c.count(), 1, "a [MISSION] with no missionfile still yields a mission");
        if (c.count() == 1) eqs(c.missions[0].stem, "somemission",
                                "...falling back to missionname for the stem");
    }

    // --- the walk stops at the first gap --------------------------------------
    // A stray later section must not reorder or extend the campaign.
    {
        ta::Campaign c;
        ta::parseCampaignText(
            "[MISSION0]\n{\nmissionfile=a.ota;\n}\n"
            "[MISSION1]\n{\nmissionfile=b.ota;\n}\n"
            "[MISSION3]\n{\nmissionfile=d.ota;\n}\n",   // 2 is missing
            "camps/x.tdf", c);
        eqi(c.count(), 2, "the mission walk stops at the first gap");
        if (c.count() == 2) eqs(c.missions[1].stem, "b", "and keeps declaration order");
    }

    // --- nothing to load ------------------------------------------------------
    {
        ta::Campaign c;
        check(!ta::parseCampaignText("", "camps/x.tdf", c),
              "empty text is not a campaign");
        check(!ta::parseCampaignText("[HEADER]\n{\ncampaignside=ARM;\n}\n",
                                     "camps/x.tdf", c),
              "nor is a header with no missions");
    }

    std::printf(g_fail ? "campaign_test: %d FAILURE(S)\n" : "campaign_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
