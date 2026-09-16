// otamission_test -- TA's campaign missions, which are .ota files.
//
// Data-independent, so CI runs it.
//
// A TA mission is not a separate file type: it is a map's .ota carrying extra
// GlobalHeader keys, its opening board as placed units, and its objectives as
// plain keys. That shape is nothing like the Kingdoms mission this engine
// inherited (a `.cob` god script plus a binary `.crt` trigger file, with the .ota
// naming player ROLES), so the rules below are the ones that had to be read off
// the shipped data and would be silent to break.
//
//   * A [Schema N] in a CAMPAIGN mission is a DIFFICULTY -- AC01 ships Easy /
//     Medium / Hard, each with its own board -- where a skirmish map's schemas
//     vary the player count. Reading only the first schema, which is all a
//     skirmish needs, silently locks the campaign to Easy.
//   * Objectives are GlobalHeader keys, and MoveUnitToRadius packs four
//     comma-separated fields into one value.
//   * What makes a file a MISSION is that it PLACES UNITS. Every one of the 272
//     shipped .ota files carries a `brief` key, and an ordinary skirmish map
//     declares `allUnitsKilled` and `destroyAllUnits` as well -- those being how a
//     normal game is won -- so both of the tempting tests call Coast To Coast a
//     campaign mission.
//   * Unit coordinates are WORLD units, like StartPos, not cells.

#include "tnt/ota.h"

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
static void eqs(const std::string& got, const std::string& want, const std::string& what) {
    bool ok = got == want;
    std::printf("  %-70s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) { std::printf("      got '%s', want '%s'\n", got.c_str(), want.c_str()); ++g_fail; }
}

// AC01's shape, trimmed: the real file is 22 KB and 103 units over three schemas.
static const char* kMission = R"OTA([GlobalHeader]
	{
	missionname=1: A Hero Returns;
	planet=Green planet;
	brief=ArmCampaign1;
	narration=arm01;
	glamour=arm01;
	lineofsight=1;
	mapping=1;
	tidalstrength=0;
	solarstrength=128;
	lavaworld=0;
	killmul=50;
	timemul=0;
	minwindspeed=300;
	maxwindspeed=2000;
	gravity=112;
	numplayers=2;
	size=5 x 6;
	useonlyunits=AC01.tdf;
	MoveUnitToRadius=ANYTYPE, 992, 656, 64;
	AllUnitsKilled=1;
	AllUnitsKilledOfType=ARMGATE;
	SCHEMACOUNT=3;
	[Schema 0]
		{
		Type=Easy;
		aiprofile=MISSIONS;
		SurfaceMetal=3;
		MohoMetal=50;
		HumanMetal=1000;
		ComputerMetal=100;
		HumanEnergy=1000;
		ComputerEnergy=100;
		[units]
			{
			[unit0]
				{
				Unitname=ARMFAV;
				Ident=;
				XPos=1099;
				YPos=85;
				ZPos=2402;
				Player=1;
				HealthPercentage=100;
				Angle=0;
				Kills=0;
				}
			[unit1]
				{
				Unitname=CORGATE;
				Ident=thegate;
				XPos=2000;
				YPos=90;
				ZPos=1500;
				Player=2;
				HealthPercentage=55;
				Angle=180;
				Kills=3;
				}
			}
		[specials]
			{
			[special0]
				{ specialwhat=StartPos1; XPos=1024; ZPos=2048; }
			}
		}
	[Schema 1]
		{
		Type=Medium;
		HumanMetal=800;
		ComputerMetal=300;
		[units]
			{
			[unit0]
				{ Unitname=ARMFAV; XPos=1099; YPos=85; ZPos=2402; Player=1; }
			[unit1]
				{ Unitname=CORGATE; XPos=2000; YPos=90; ZPos=1500; Player=2; }
			[unit2]
				{ Unitname=CORAK; XPos=2100; YPos=90; ZPos=1600; Player=2; }
			}
		}
	[Schema 2]
		{
		Type=Hard;
		HumanMetal=500;
		ComputerMetal=900;
		[units]
			{
			[unit0]
				{ Unitname=CORAK; XPos=2100; YPos=90; ZPos=1600; Player=2; }
			}
		}
	}
)OTA";

// A skirmish map: a brief key, the ordinary win conditions, start positions, no
// placed units.
static const char* kSkirmish = R"OTA([GlobalHeader]
	{
	missionname=Coast to Coast;
	brief=;
	AllUnitsKilled=1;
	DestroyAllUnits=1;
	numplayers=2, 4;
	[Schema 0]
		{
		Type=Network 1;
		aiprofile=SeaBattle;
		SurfaceMetal=5;
		MohoMetal=40;
		[specials]
			{
			[special0]
				{ specialwhat=StartPos3; XPos=3010; ZPos=1497; }
			[special1]
				{ specialwhat=StartPos1; XPos=134; ZPos=236; }
			}
		}
	}
)OTA";

int main() {
    std::printf("otamission_test\n");

    // --- a campaign mission ---------------------------------------------------
    {
        auto s = ta::tnt::Scenario::parse(kMission);
        check(s.isMission(), "a file that places units reads as a mission");
        eqs(s.missionName, "1: A Hero Returns", "the mission name parses");
        eqs(s.brief, "ArmCampaign1", "and its briefing id");
        eqs(s.narration, "arm01", "and its narration");
        eqs(s.planet, "Green planet", "and its planet");
        eqs(s.useOnlyUnits, "AC01.tdf", "and the unit restriction file");
        eqi(s.killMul, 50, "killmul");
        eqi(s.mapping, 1, "mapping");

        // Three schemas, and they are DIFFICULTIES with their own boards.
        eqi(int(s.schemas.size()), 3, "all three schemas parse, not just the first");
        if (s.schemas.size() == 3) {
            eqs(s.schemas[0].type, "Easy", "schema 0 is Easy");
            eqs(s.schemas[2].type, "Hard", "schema 2 is Hard");
            eqi(int(s.schemas[0].units.size()), 2, "Easy's board");
            eqi(int(s.schemas[1].units.size()), 3, "Medium's board is its OWN, not Easy's");
            eqi(int(s.schemas[2].units.size()), 1, "and Hard's");
            // The difficulty knob is the asymmetry in the opening treasuries.
            eqi(int(s.schemas[0].humanMetal), 1000, "Easy hands the human 1000 metal");
            eqi(int(s.schemas[0].computerMetal), 100, "...against the computer's 100");
            eqi(int(s.schemas[2].computerMetal), 900, "Hard reverses it");
        }

        // schemaFor picks by difficulty name, case-insensitively, and falls back.
        const auto* hard = s.schemaFor("hard");
        check(hard && hard->type == "Hard", "schemaFor matches a difficulty case-insensitively");
        const auto* miss = s.schemaFor("Nonexistent");
        check(miss && miss->type == "Easy", "an unknown difficulty falls back to the first");
        const auto* none = s.schemaFor("");
        check(none && none->type == "Easy", "so does an empty one");

        // A placed unit, in WORLD units.
        if (!s.schemas.empty() && s.schemas[0].units.size() == 2) {
            const auto& u0 = s.schemas[0].units[0];
            eqs(u0.unitName, "ARMFAV", "unit 0's type");
            eqi(u0.xpos, 1099, "its X, unscaled (world units, not cells)");
            eqi(u0.ypos, 85, "its Y");
            eqi(u0.zpos, 2402, "its Z");
            eqi(u0.player, 1, "its player");
            const auto& u1 = s.schemas[0].units[1];
            eqs(u1.ident, "thegate", "a unit's script handle parses");
            eqi(u1.healthPercent, 55, "a damaged unit's health");
            // DEGREES, 0..359 -- measured across every Angle in the shipped
            // .ota corpus: 282 distinct values, max 359, none above.
            eqi(u1.angle, 180, "its heading, in degrees");
            eqi(u1.kills, 3, "and the veterancy it starts with");
            eqi(s.schemas[0].units[0].healthPercent, 100,
                "a unit that states no health defaults to full");
        }

        // Objectives, including the packed one.
        const auto& o = s.objectives;
        check(o.any(), "the mission declares objectives");
        check(o.hasMoveToRadius, "MoveUnitToRadius is recognised");
        eqs(o.moveToType, "ANYTYPE", "...its type field");
        eqi(o.moveToX, 992, "its X");
        eqi(o.moveToZ, 656, "its Z");
        eqi(o.moveToRadius, 64, "and its radius, from one comma-separated value");
        check(!o.commanderKilled, "an objective it does not declare stays off");

        // THE WIN/LOSE SPLIT. AC01 declares AllUnitsKilled and
        // AllUnitsKilledOfType=ARMGATE, and BOTH are defeat conditions -- the only
        // ARMGATE on that map belongs to the player, so the key means "protect the
        // gate". Classifying either as a win inverts the mission.
        check(o.allUnitsKilled, "AllUnitsKilled parses");
        eqs(o.allUnitsKilledOfType, "ARMGATE", "AllUnitsKilledOfType names its unit");
        check(o.anyLose(), "...and both count as LOSE conditions");
        check(o.hasMoveToRadius && o.anyWin(),
              "while MoveUnitToRadius is the WIN condition");
        // Declaring only defeat conditions must not read as declaring a victory.
        {
            auto d = ta::tnt::Scenario::parse(
                "[GlobalHeader]\n{\nCommanderKilled=1;\nAllUnitsKilled=1;\n"
                "DeathTimerRunsOut=1;\nUnitTypeKilled=ARMCOM;\n}\n");
            check(d.objectives.anyLose(), "a lose-only mission has defeat conditions");
            check(!d.objectives.anyWin(), "...and no victory conditions at all");
        }
        // ...and the mirror: the win-side keys, including the two the first pass
        // at this missed entirely (KillEnemyCommander is a separate key from
        // CommanderKilled, and sits on the other list).
        {
            auto w = ta::tnt::Scenario::parse(
                "[GlobalHeader]\n{\nKillEnemyCommander=1;\nVictoryTimerRunsOut=1;\n"
                "DestroyAllUnits=1;\nKillAllOfType=CORGATE;\nBuildUnitType=ARMLAB;\n"
                "CaptureUnitType=CORCOM;\nAnyUnitPassesX=1200;\n}\n");
            check(w.objectives.killEnemyCommander, "KillEnemyCommander parses");
            check(w.objectives.victoryTimerRunsOut, "VictoryTimerRunsOut parses");
            eqs(w.objectives.killAllOfType, "CORGATE", "KillAllOfType");
            eqs(w.objectives.buildUnitType, "ARMLAB", "BuildUnitType");
            eqs(w.objectives.captureUnitType, "CORCOM", "CaptureUnitType");
            check(w.objectives.anyWin(), "all of those are WIN conditions");
            check(!w.objectives.commanderKilled,
                  "KillEnemyCommander does NOT set CommanderKilled -- different keys, "
                  "different lists");
            check(w.objectives.hasAnyUnitPassesX && w.objectives.anyUnitPassesX == 1200,
                  "AnyUnitPassesX parses its line");
            check(w.objectives.anyLose(), "...and is a LOSE condition");
        }
    }

    // --- a skirmish map is NOT a mission --------------------------------------
    {
        auto s = ta::tnt::Scenario::parse(kSkirmish);
        check(!s.isMission(), "a map that places no units is not a mission");
        // ...even though both of the tempting tests would say otherwise:
        check(s.objectives.any(),
              "even though it declares objectives (that is how a skirmish is won)");
        eqi(int(s.schemas.size()), 1, "its single schema parses");
        // Start positions still come through, ordered by StartPos number.
        if (!s.schemas.empty()) {
            eqi(int(s.schemas[0].starts.size()), 2, "its start positions parse");
            if (s.schemas[0].starts.size() == 2)
                eqi(s.schemas[0].starts[0].number, 1,
                    "and sort by StartPos number, not declaration order");
        }
        eqi(int(s.schemas[0].surfaceMetal), 5, "the schema's metal richness");
    }

    // --- malformed input ------------------------------------------------------
    {
        auto s = ta::tnt::Scenario::parse("");
        check(s.schemas.empty() && !s.isMission(), "empty input yields no schemas");
        check(s.schemaFor("Easy") == nullptr, "and schemaFor returns nothing rather than crashing");

        // A MoveUnitToRadius with too few fields is ignored, not half-applied.
        auto t = ta::tnt::Scenario::parse(
            "[GlobalHeader]\n{\nMoveUnitToRadius=ANYTYPE, 10;\n}\n");
        check(!t.objectives.hasMoveToRadius,
              "a truncated MoveUnitToRadius is ignored rather than half-read");

        // A unit with no Unitname is skipped rather than placed as a blank.
        auto u = ta::tnt::Scenario::parse(
            "[GlobalHeader]\n{\n[Schema 0]\n{\n[units]\n{\n"
            "[unit0]\n{ XPos=10; ZPos=20; Player=1; }\n"
            "[unit1]\n{ Unitname=ARMPW; XPos=30; ZPos=40; Player=1; }\n"
            "}\n}\n}\n");
        check(u.schemas.size() == 1 && u.schemas[0].units.size() == 1,
              "a unit block with no Unitname is skipped, and the next one still parses");
        if (u.schemas.size() == 1 && u.schemas[0].units.size() == 1)
            eqs(u.schemas[0].units[0].unitName, "ARMPW", "and it is the right one");
    }

    std::printf(g_fail ? "otamission_test: %d FAILURE(S)\n" : "otamission_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
