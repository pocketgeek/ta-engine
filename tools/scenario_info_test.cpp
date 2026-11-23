// scenario_info_test -- MAP SCENARIO (protocol 259): sim::inspectScenario reads a map's
// scenario facts from its .ota/.crt/.tdf, and agrees with setupMatch.
//
// The inspector decides what the CHANGE MAP panel offers, which seats the room tags and
// requires, and what the server accepts at START, without building a world. setupMatch
// remains the authority, so the matrix at the end runs every case through both: with a
// registry, status Ok exactly when setupMatch (MatchConfig::mapScenario, every required
// seat used) plays the scenario; the refusal statuses exactly when it refuses; the
// statuses that are not scenarios (none declared, leftover data) exactly when it plays a
// plain skirmish. Placement bounds are the one check left to setupMatch (documented in
// scenarioinfo.h), and the matrix pins that exception too.

#include "crt/crt.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/scenarioinfo.h"
#include "sim/sim.h"
#include "tnt/ota.h"
#include "tnt/tnt.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace tak;
using sim::ScenarioStatus;

static int g_fail = 0, g_checks = 0;
static void check(bool cond, const std::string& what, const std::string& detail = {}) {
    ++g_checks;
    if (!cond) { ++g_fail; std::printf("  FAIL %s%s%s\n", what.c_str(), detail.empty() ? "" : " -- ", detail.c_str()); }
}

static const char* statusName(ScenarioStatus s) {
    switch (s) {
        case ScenarioStatus::Ok: return "Ok";
        case ScenarioStatus::NoScenario: return "NoScenario";
        case ScenarioStatus::Leftover: return "Leftover";
        case ScenarioStatus::CrtMissing: return "CrtMissing";
        case ScenarioStatus::CrtDamaged: return "CrtDamaged";
        case ScenarioStatus::Empty: return "Empty";
        case ScenarioStatus::BadOwner: return "BadOwner";
        case ScenarioStatus::UseOnlyBroken: return "UseOnlyBroken";
        case ScenarioStatus::MissingType: return "MissingType";
    }
    return "?";
}

namespace {
using Files = hpi::Vfs::Files;
const std::string kMap = "kmap/case.tnt", kOta = "kmap/case.ota", kCrt = "kmap/case.crt", kTdf = "kmap/case.tdf";

std::vector<uint8_t> bytes(const std::string& text) { return {text.begin(), text.end()}; }

// One synthetic map: the shared unit types and terrain plus this case's companions.
struct Case {
    std::string name;
    tnt::Scenario ota;               // the .ota's GlobalHeader (hasScenario, useOnlyUnits)
    bool marker = false;             // [TAKPlaytest] authoredscenario=1 (a Test Map copy)
    bool crt = false;                // write a .crt ...
    crt::Scenario scenario;          // ... of this scenario
    std::vector<uint8_t> rawCrt;     // ... or these bytes, when set
    std::map<std::string, std::string> extra;   // further companions (path -> text)
};

crt::Scenario emptyScenario() { crt::Scenario s; s.players.resize(9); return s; }
crt::Unit unit(const char* type, int player, int x = 10, int z = 10) {
    crt::Unit u; u.objectName = type; u.player = player; u.x = x; u.z = z; return u;
}
crt::RuleGroup group(std::vector<crt::Rule> conditions, std::vector<crt::Rule> actions) {
    crt::RuleGroup g; g.conditions = std::move(conditions); g.actions = std::move(actions); return g;
}
}  // namespace

int main() {
    std::printf("scenario_info_test\n");
    auto base = std::make_shared<Files>();
    for (const char* name : {"soldier", "builder", "araking", "tarnecro"}) {
        const auto text = std::string("[UNITINFO]{\nunitname=") + name + ";\nobjectname=" + name +
            ";\nname=Display " + name + ";\nmaxdamage=100;\nfootprintx=1;\nfootprintz=1;\ncanmove=1;\nmaxvelocity=1;\n}";
        (*base)[std::string("units/") + name + ".fbi"] = bytes(text);
    }
    // What a seat cannot command: wildlife (side LIF, mobile) and an unarmed kingdom
    // building that builds nothing (a lodestone).
    (*base)["units/cow.fbi"] = bytes("[UNITINFO]{\nunitname=cow;\nobjectname=cow;\nside=LIF;\nmaxdamage=50;\n"
                                     "footprintx=1;\nfootprintz=1;\ncanmove=1;\nmaxvelocity=1;\n}");
    (*base)["units/lodestone.fbi"] = bytes("[UNITINFO]{\nunitname=lodestone;\nobjectname=lodestone;\nside=ARA;\n"
                                           "maxdamage=500;\nfootprintx=2;\nfootprintz=2;\nmaxvelocity=0;\n}");
    tnt::Map map; map.width = map.height = 64; map.blocksX = map.blocksY = 32;
    map.heights.assign(4096, 40); map.features.assign(4096, 0xffff);
    map.tileKeys.assign(1024, 0); map.tileCols.assign(1024, 0); map.tileRows.assign(1024, 0);
    (*base)[kMap] = map.save();
    sim::TypeRegistry registry;
    { hpi::Vfs units; units.setMapFiles(base); registry.loadDir(units, "units"); }
    check(registry.find("soldier") && registry.find("araking") && registry.find("cow") && registry.find("lodestone") &&
          registry.find("lodestone")->isStructure() && registry.find("cow")->side == "LIF", "synthetic unit types load");

    auto filesOf = [&](const Case& c) {
        auto files = std::make_shared<Files>(*base);
        std::string ota = c.ota.write();
        if (c.marker) ota += "\n[TAKPlaytest]\n{\nauthoredscenario=1;\n}\n";
        (*files)[kOta] = bytes(ota);
        if (!c.rawCrt.empty()) (*files)[kCrt] = c.rawCrt;
        else if (c.crt) (*files)[kCrt] = crt::write(c.scenario);
        for (const auto& [path, text] : c.extra) (*files)[path] = bytes(text);
        return files;
    };
    auto inspect = [&](const Case& c, bool withRegistry = true) {
        hpi::Vfs view; view.setMapFiles(filesOf(c));
        return sim::inspectScenario(view, kMap, withRegistry ? &registry : nullptr);
    };

    // A declared scenario with content: two placed units, a Victory rule for Player 1.
    Case good; good.name = "declared scenario"; good.ota.hasScenario = true; good.crt = true;
    good.scenario = emptyScenario();
    good.scenario.units = {unit("SOLDIER", 0), unit("builder", 1, 30, 30)};
    good.scenario.players[1].push_back(group({{0, {}}}, {{5, {}}}));

    std::vector<std::pair<Case, ScenarioStatus>> matrix;
    auto expect = [&](const Case& c, ScenarioStatus want) {
        const auto info = inspect(c);
        check(info.status == want, c.name + ": status", std::string("got ") + statusName(info.status) + ", want " + statusName(want));
        matrix.push_back({c, want});
        return info;
    };

    std::printf("status:\n");
    {
        auto info = expect(good, ScenarioStatus::Ok);
        check(info.runnable() && info.content() && info.offerable() && !info.marker, "a declared scenario is offerable");
        check(info.active(true) && !info.active(false), "an offerable map plays its scenario only with the option");
        check(info.placed == 2 && info.placedSeats == 0b11 && info.ruleSeats == 0b1 && info.outcomes &&
              info.useOnlyTypes == -1 && info.requiredSeats() == 0b11, "declared scenario facts");
    }
    {
        Case c; c.name = "no scenario, no CRT"; expect(c, ScenarioStatus::NoScenario);
        c.name = "no scenario, empty CRT"; c.crt = true; c.scenario = emptyScenario();
        expect(c, ScenarioStatus::NoScenario);
    }
    {
        Case c = good; c.name = "leftover (hasscenario=0 with CRT content)"; c.ota.hasScenario = false;
        auto info = expect(c, ScenarioStatus::Leftover);
        check(!info.runnable() && !info.offerable() && !info.active(true),
              "leftover data is never runnable, offered or played");
        check(info.placed == 2 && info.requiredSeats() == 0b11, "leftover facts still filled");
    }
    {
        Case c = good; c.name = "declared, no CRT"; c.crt = false;
        auto info = expect(c, ScenarioStatus::CrtMissing);
        check(!info.runnable() && !info.offerable(), "a missing CRT is not runnable");
        c.name = "declared, damaged CRT"; c.rawCrt = {1, 2, 3};
        info = expect(c, ScenarioStatus::CrtDamaged);
        check(!info.crtValid && !info.runnable(), "a damaged CRT is not runnable");
    }

    std::printf("empty scenarios:\n");
    {
        Case c = good; c.scenario = emptyScenario();
        c.name = "one empty group in bank 8"; c.scenario.players[8].push_back({});
        auto info = expect(c, ScenarioStatus::Empty);
        check(info.requiredSeats() == 0 && !info.content() && !info.offerable(), "an empty rule bank needs no seat");
        c.scenario = emptyScenario(); c.name = "a region only"; c.scenario.regions.push_back({"Hill", 1, 1, 4, 4});
        info = expect(c, ScenarioStatus::Empty);
        check(info.requiredSeats() == 0, "a region needs no seat");
        c.scenario = emptyScenario(); c.name = "custom types only"; c.scenario.customTypes.push_back({"soldier", {200, 100, 100, 0}});
        info = expect(c, ScenarioStatus::Empty);
        check(info.requiredSeats() == 0, "custom stats need no seat");
    }

    std::printf("seats:\n");
    {
        Case c = good; c.name = "owners 0 and 2, rules in bank 4, an empty group in bank 7";
        c.scenario = emptyScenario();
        c.scenario.units = {unit("soldier", 0), unit("soldier", 2, 20, 20)};
        c.scenario.players[4].push_back(group({{0, {}}}, {{2, {"flag", "1"}}}));
        c.scenario.players[7].push_back({});
        auto info = expect(c, ScenarioStatus::Ok);
        check(info.requiredSeats() == 0b00001101, "required seats = placement owners + banks with rules",
              std::to_string(info.requiredSeats()));
        check(info.placedSeats == 0b101 && info.ruleSeats == 0b1000, "seat masks split by source");
    }
    {
        Case c = good; c.name = "shared (All Players) rules only"; c.scenario = emptyScenario();
        c.scenario.players[0].push_back(group({{0, {}}}, {{13, {"All Players", "hello"}}}));
        auto info = expect(c, ScenarioStatus::Ok);
        check(info.content() && info.sharedRules && info.requiredSeats() == 0 && info.placed == 0,
              "shared rules are content that needs no seat");
    }
    {
        Case c = good; c.name = "neutral placements only"; c.scenario = emptyScenario();
        c.scenario.units = {unit("soldier", 8)};
        auto info = expect(c, ScenarioStatus::Ok);
        check(info.content() && info.requiredSeats() == 0 && info.placedSeats == 0, "the neutral owner needs no seat");
        check(info.armySeats == 0 && !info.offerable(), "neutral placements leave every seat without an army");
    }

    std::printf("player 1's army:\n");
    {
        // Placements replace every Monarch, so player 1 -- the room's creator -- must get
        // something to command. 39 downloaded maps place only wildlife or decoration for it.
        Case c = good; c.name = "wildlife for player 1"; c.scenario = emptyScenario();
        c.scenario.units = {unit("cow", 0), unit("soldier", 1, 30, 30)};
        auto info = expect(c, ScenarioStatus::Ok);
        check(info.armySeats == 0b10 && !info.seatOneArmy() && !info.offerable() && info.requiredSeats() == 0b11,
              "wildlife is not an army", std::to_string(info.armySeats));
        std::string why;
        for (const auto& line : sim::scenarioReason(info)) why += (why.empty() ? "" : "|") + line;
        check(why == "PLAYER 1 GETS NO ARMY|(NO FIGHTER OR BUILDER)", "no-army reason", why);
        check(inspect(c, false).offerable() && inspect(c, false).armySeats == 0xFF, "the army is a registry check");
        c.name = "a lodestone for player 1"; c.scenario.units[0] = unit("lodestone", 0);
        check(!expect(c, ScenarioStatus::Ok).offerable(), "an unarmed building that builds nothing is not an army");
        c.name = "nothing for player 1"; c.scenario.units = {unit("soldier", 1, 30, 30)};
        info = expect(c, ScenarioStatus::Ok);
        why.clear();
        for (const auto& line : sim::scenarioReason(info)) why += (why.empty() ? "" : "|") + line;
        check(!info.offerable() && why == "PLAYER 1 GETS NO ARMY|(NO UNITS PLACED FOR IT)", "player 1 with no placement", why);
        c.name = "player 1's rules create its army";
        c.scenario.players[1].push_back(group({{0, {}}}, {{7, {"soldier", "Hill"}}}));
        info = expect(c, ScenarioStatus::Ok);
        check(info.armySeats == 0b11 && info.offerable(), "a Create rule in player 1's bank is an army");
        c.name = "All Players' rules create armies"; c.scenario.players[1].clear();
        c.scenario.players[0].push_back(group({{0, {}}}, {{7, {"soldier", "Hill"}}}));
        info = expect(c, ScenarioStatus::Ok);
        check(info.armySeats == 0xFF && info.offerable(), "an All Players Create rule arms every seat");
        c = good; c.name = "rules only"; c.scenario.units.clear();
        info = expect(c, ScenarioStatus::Ok);
        check(info.seatOneArmy() && info.offerable(), "a rules-only map keeps its Monarchs");
        c = good; c.name = "player 1's army is a type this install lacks"; c.scenario.units[0].objectName = "npckrell";
        info = expect(c, ScenarioStatus::MissingType);
        check(info.seatOneArmy(), "an unknown type may come from a game mod pack");
    }

    std::printf("outcomes:\n");
    {
        Case c = good; c.name = "Defeat me + teammates (action 22)"; c.scenario = emptyScenario();
        c.scenario.units = {unit("soldier", 0)};
        c.scenario.players[1].push_back(group({{0, {}}}, {{22, {}}}));
        check(expect(c, ScenarioStatus::Ok).outcomes, "action 22 is an outcome");
        c.name = "condition 21 with action 2 only"; c.scenario.players[1].clear();
        c.scenario.players[1].push_back(group({{21, {"0"}}}, {{2, {"flag", "1"}}}));
        check(!expect(c, ScenarioStatus::Ok).outcomes, "condition opcodes are not outcomes");
    }

    std::printf("use only:\n");
    {
        Case c = good; c.name = "Use Only names another file"; c.ota.useOnlyUnits = "other.tdf";
        c.extra["kmap/other.tdf"] = "[SOLDIER]{}\n";
        auto info = expect(c, ScenarioStatus::UseOnlyBroken);
        check(info.detail == "other.tdf" && info.useOnlyTypes == -1 && !info.useOnlyMissing,
              "broken Use Only detail (misnamed, so setupMatch names it: not missing)");
        c.name = "Use Only outside the map folder"; c.ota.useOnlyUnits = "../case.tdf"; c.extra = {{kTdf, "[SOLDIER]{}\n"}};
        expect(c, ScenarioStatus::UseOnlyBroken);
        c.name = "Use Only file missing"; c.ota.useOnlyUnits = "case.tdf"; c.extra.clear();
        info = expect(c, ScenarioStatus::UseOnlyBroken);
        check(info.useOnlyMissing, "a missing Use Only file is flagged");
        c.name = "Use Only file does not parse"; c.extra = {{kTdf, "[SOLDIER]{\n"}};
        expect(c, ScenarioStatus::UseOnlyBroken);
        // MAP SCENARIO leaves out a name no unit answers to (NeverGlory names 44 such);
        // a Test Map copy refuses it as the author's typo.
        c.name = "Use Only names an unknown unit"; c.extra = {{kTdf, "[SOLDIER]{}\n[NPCKRELL]{}\n"}};
        info = expect(c, ScenarioStatus::Ok);
        check(info.useOnlyTypes == 1 && info.offerable(), "an unknown Use Only name is left out of the count",
              std::to_string(info.useOnlyTypes));
        info = inspect(c, false);
        check(info.status == ScenarioStatus::Ok && info.useOnlyTypes == 2, "without a registry every entry counts");
        c.name = "Test Map copy's Use Only names an unknown unit"; c.marker = true;
        info = expect(c, ScenarioStatus::MissingType);
        check(info.detail == "npckrell" && info.useOnlyTypes == 2, "unknown Use Only unit detail (Test Map copy)");
        c.marker = false;
        c.name = "empty Use Only file"; c.extra = {{kTdf, ""}};
        info = expect(c, ScenarioStatus::Ok);
        check(info.useOnlyTypes == 0, "an empty Use Only list allows nothing");
        c.name = "Use Only of two"; c.extra = {{kTdf, "[SOLDIER]{}\n[builder]{}\n"}};
        info = expect(c, ScenarioStatus::Ok);
        check(info.useOnlyTypes == 2 && !info.useOnlyMissing, "Use Only entry count");
    }
    {
        // useOnlyMissing is set whatever the status: the server refuses a package that
        // leaves the list out whenever setupMatch would read it (a Test Map copy's list
        // applies without a declared scenario), since each machine would read its own.
        Case c; c.name = "Test Map copy, Use Only only, file missing"; c.marker = true; c.ota.useOnlyUnits = "case.tdf";
        auto info = inspect(c);
        check(info.status == ScenarioStatus::NoScenario && info.useOnlyMissing && info.marker, "flag behind NoScenario");
        c.name = "Test Map copy, Use Only only"; c.extra = {{kTdf, "[SOLDIER]{}\n[builder]{}\n"}};
        info = inspect(c);
        check(info.marker && !info.hasScenario && !info.useOnlyMissing && info.useOnlyTypes == 2,
              "a Test Map copy's list is described without a scenario");
        c = good; c.name = "leftover, Use Only file missing"; c.ota.hasScenario = false; c.ota.useOnlyUnits = "case.tdf";
        info = inspect(c);
        check(info.status == ScenarioStatus::Leftover && info.useOnlyMissing, "flag behind Leftover");
        check(!inspect(good).useOnlyMissing, "no list, no flag");
    }
    {
        Case c = good; c.name = "placed unit type unknown"; c.scenario.units[1].objectName = "npckrell";
        auto info = expect(c, ScenarioStatus::MissingType);
        check(info.detail == "npckrell", "unknown placed unit detail");
        check(inspect(c, false).status == ScenarioStatus::Ok, "placed types are checked only with a registry");
        // setupMatch skips a custom type no unit answers to (as retail does), so it is offered.
        c = good; c.name = "custom type unknown"; c.scenario.customTypes.push_back({"npckrell", {200, 100, 100, 0}});
        expect(c, ScenarioStatus::Ok);
    }
    {
        Case c = good; c.name = "owner 9"; c.scenario.units.push_back(unit("soldier", 9));
        expect(c, ScenarioStatus::BadOwner);
        c = good; c.name = "rules past the neutral bank"; c.scenario.players.resize(11);
        c.scenario.players[10].push_back({});
        expect(c, ScenarioStatus::BadOwner);
    }
    {
        // setupMatch refuses an off-map placement; the inspector leaves that to it (the
        // map list would otherwise decompress every scenario map's terrain).
        Case c = good; c.name = "placement off the map"; c.scenario.units[1].x = 64;
        check(inspect(c).status == ScenarioStatus::Ok, "placement bounds are setupMatch's check");
    }

    std::printf("test map copies:\n");
    {
        Case c = good; c.name = "Test Map copy"; c.marker = true;
        auto info = inspect(c);
        check(info.marker && info.status == ScenarioStatus::Ok && !info.offerable(),
              "a Test Map copy is never offered (its opt-in plays it)");
        check(info.active(false) && info.active(true), "a runnable Test Map copy always plays");
        c.ota.hasScenario = false; info = inspect(c);
        check(info.marker && !info.runnable() && !info.active(false), "a Test Map copy without a declared scenario plays none");
        c = good; c.name = "empty Test Map copy"; c.marker = true; c.scenario = emptyScenario(); info = inspect(c);
        check(info.status == ScenarioStatus::Empty && info.active(false), "an empty Test Map copy still runs");
    }
    {
        Case c = good; c.name = "unparseable .ota";
        auto files = filesOf(c); (*files)[kOta] = bytes("[GlobalHeader]{\nhasscenario=1;\n");
        hpi::Vfs view; view.setMapFiles(files);
        check(sim::inspectScenario(view, kMap, &registry).status == ScenarioStatus::NoScenario,
              "an .ota that does not parse has no scenario (and does not throw)");
        hpi::Vfs none;
        check(sim::inspectScenario(none, kMap, &registry).status == ScenarioStatus::NoScenario, "no map at all");
    }

    std::printf("seat lists and text:\n");
    check(sim::seatList(0b00001111) == "1-4", "seatList 1-4", sim::seatList(0b00001111));
    check(sim::seatList(0b00000101) == "1 3", "seatList 1 3", sim::seatList(0b00000101));
    check(sim::seatList(0b11110010) == "2 5-8", "seatList 2 5-8", sim::seatList(0b11110010));
    check(sim::seatList(0b00000011) == "1 2" && sim::seatList(0) == "" && sim::seatList(0xff) == "1-8", "seatList runs");
    auto joined = [](const auto& lines) {
        std::string out;
        for (const auto& line : lines) {
            if constexpr (std::is_same_v<std::decay_t<decltype(line)>, sim::ScenarioLine>)
                out += (out.empty() ? "" : "|") + line.text + (line.warning ? "!" : "");
            else out += (out.empty() ? "" : "|") + line;
        }
        return out;
    };
    {
        Case c = good; c.ota.hasScenario = false;
        check(joined(sim::scenarioReason(inspect(c))) == "THIS MAP HAS NO SCENARIO|(UNUSED LEFTOVER RULES)", "leftover reason");
        c = good; c.scenario.units[1].objectName = "averyveryverylongunitnamethatgoeson";
        const auto why = joined(sim::scenarioReason(inspect(c)));
        check(why == "NEEDS A UNIT NOT INSTALLED|AVERYVERYVERYLONGUNITNAMET", "missing type reason (uppercased, 26 chars)", why);
        check(sim::scenarioReason(inspect(good)).empty(), "an offerable map has no reason");
        sim::ScenarioInfo uo; uo.useOnlyTypes = 6;
        check(sim::scenarioUseOnlyLine(uo, true) == "BUILDS 6 UNIT TYPES" &&
              sim::scenarioUseOnlyLine(uo, false) == "ONLY 6 UNIT TYPES CAN BE BUILT", "Use Only line");
        uo.useOnlyTypes = 1;
        check(sim::scenarioUseOnlyLine(uo, true) == "BUILDS 1 UNIT TYPE", "Use Only line, one type");
        uo.useOnlyTypes = 0;
        check(sim::scenarioUseOnlyLine(uo, true) == "NOTHING CAN BE BUILT" &&
              sim::scenarioUseOnlyLine(uo, false) == "NO UNIT TYPE CAN BE BUILT", "Use Only line, nothing");
        uo.useOnlyTypes = -1;
        check(sim::scenarioUseOnlyLine(uo, true).empty() &&
              sim::scenarioUseOnlyLine(uo, false) == "ALL UNIT TYPES CAN BE BUILT", "Use Only line, unrestricted");
    }
    {
        // King of the Hill: four seats with placed armies, a Use Only list of six, outcomes.
        sim::ScenarioInfo koth;
        koth.status = ScenarioStatus::Ok; koth.hasScenario = true; koth.crtValid = true;
        koth.placed = 40; koth.placedSeats = 0x0F; koth.ruleSeats = 0x0F; koth.outcomes = true; koth.useOnlyTypes = 6;
        check(joined(sim::scenarioSummary(koth, true)) ==
              "UNITS PLACED FOR 1-4|NEEDS SEATS 1-4|BUILDS 6 UNIT TYPES|ENDS BY SCENARIO RULES", "compact summary",
              joined(sim::scenarioSummary(koth, true)));
        check(joined(sim::scenarioSummary(koth, false)) ==
              "PLACED UNITS REPLACE MONARCHS|NEEDS SEATS 1-4|ONLY 6 UNIT TYPES CAN BE BUILT|ENDS ONLY BY THE SCENARIO'S RULES",
              "full summary", joined(sim::scenarioSummary(koth, false)));
        auto rules = koth; rules.placed = 0; rules.placedSeats = 0; rules.outcomes = false; rules.useOnlyTypes = -1;
        check(joined(sim::scenarioSummary(rules, true)) ==
              "RULES ONLY: MONARCHS STAY|NEEDS SEATS 1-4|NO VICTORY RULES: ENDLESS!", "rules-only compact summary",
              joined(sim::scenarioSummary(rules, true)));
        check(joined(sim::scenarioSummary(rules, false)) ==
              "RULES ONLY: EVERYONE KEEPS A MONARCH|NEEDS SEATS 1-4|ALL UNIT TYPES CAN BE BUILT|NO VICTORY RULES: IT NEVER ENDS BY ITSELF!",
              "rules-only full summary", joined(sim::scenarioSummary(rules, false)));
        auto none = koth; none.useOnlyTypes = 0; none.placedSeats = 0b11011011; none.ruleSeats = 0;
        check(joined(sim::scenarioSummary(none, true)) ==
              "UNITS PLACED FOR 6 SEATS|NEEDS SEATS 1 2 4 5 7 8|NOTHING CAN BE BUILT|ENDS BY SCENARIO RULES",
              "scattered seats and an empty Use Only list", joined(sim::scenarioSummary(none, true)));
        for (bool compact : {true, false})
            for (const auto* info : {&koth, &rules, &none})
                for (const auto& line : sim::scenarioSummary(*info, compact))
                    check(line.text.size() <= (compact ? 26u : 44u), "summary line fits its column", line.text);
    }

    // The agreement matrix: every case above through setupMatch with the option on and
    // every seat the inspector says the scenario needs (and only those) in use.
    std::printf("agreement with setupMatch:\n");
    auto run = [&](const Case& c, uint8_t seats, std::string& error) {
        auto files = filesOf(c);
        hpi::Vfs vfs; vfs.setMapFiles(files);
        sim::MatchConfig cfg; cfg.vfs = &vfs; cfg.mapPath = kMap; cfg.mapScenario = true;
        int count = 2;
        for (int i = 0; i < sim::kMaxPlayers; ++i) if (seats >> i & 1) count = std::max(count, i + 1);
        cfg.slots.resize(size_t(count));
        for (int i = 0; i < count; ++i)
            cfg.slots[size_t(i)] = {seats ? bool(seats >> i & 1) : i < 2, i % 2, i};
        auto world = std::make_unique<sim::World>();
        try { sim::setupMatch(*world, registry, cfg); }
        catch (const std::exception& e) { error = e.what(); return std::unique_ptr<sim::World>(); }
        return world;
    };
    for (const auto& [c, want] : matrix) {
        const auto info = inspect(c);
        std::string error;
        auto world = run(c, info.requiredSeats(), error);
        const bool refused = !world;
        switch (want) {
            case ScenarioStatus::Ok:
                check(!refused && world->scenario(), c.name + ": Ok but setupMatch did not play it", error);
                break;
            case ScenarioStatus::CrtMissing: case ScenarioStatus::CrtDamaged: case ScenarioStatus::BadOwner:
            case ScenarioStatus::UseOnlyBroken: case ScenarioStatus::MissingType:
                check(refused, c.name + ": refused by the inspector but setupMatch started it");
                break;
            case ScenarioStatus::NoScenario: case ScenarioStatus::Leftover:
                check(!refused && !world->scenario(), c.name + ": no scenario, but setupMatch did not play a skirmish", error);
                break;
            case ScenarioStatus::Empty:   // never offered; the server refuses START for it
                break;
        }
        // Each required seat is required: leave it empty and setupMatch names it.
        for (int i = 0; want == ScenarioStatus::Ok && i < sim::kMaxPlayers; ++i)
            if (info.requiredSeats() >> i & 1) {
                std::string why;
                run(c, uint8_t(info.requiredSeats() & ~(1u << i)) | uint8_t(i == 0 ? 0b10 : 0b1), why);
                check(why == "Seat player " + std::to_string(i + 1) + " in the lobby to play this scenario",
                      c.name + ": seat " + std::to_string(i + 1) + " is required", why);
            }
    }
    {
        Case c = good; c.name = "placement off the map"; c.scenario.units[1].x = 64;
        std::string error;
        check(!run(c, inspect(c).requiredSeats(), error) && error.find("outside the map") != std::string::npos,
              "the documented exception: setupMatch refuses an off-map placement", error);
    }

    if (g_fail) std::printf("scenario_info_test: %d of %d checks FAILED\n", g_fail, g_checks);
    else std::printf("scenario_info_test: all %d checks passed\n", g_checks);
    return g_fail ? 1 : 0;
}
