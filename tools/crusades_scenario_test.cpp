// A map scenario played with the CRUSADES balance, whose roster (unitscb/ alone) has no
// monelem, monmum, tarcship, tarhel, tarnecr2 or zonspide. Needs a retail install.
//
//   crusades_scenario_test <install>                 the checks below
//   crusades_scenario_test <install> --survey <root>  list <root>'s maps whose scenario
//                                                     loses units with CRUSADES on
//
// Checks: a custom type Crusades lacks is skipped and the scenario starts (retail
// 0x4cd243); a placed one is refused with a message naming it and CRUSADES; Check Map
// (cart::validateCrusadesRoster) warns about it on the real rosters.
#include "cartographer/units.h"
#include "cartographer/validation.h"
#include "crt/crt.h"
#include "hpi/hpi.h"
#include "net/mappackage.h"
#include "sim/matchsetup.h"
#include "sim/scenarioinfo.h"
#include "tnt/ota.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace tak;
static void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

static int survey(const char* install, const char* root) {
    auto base = hpi::mountRetailRoot(install);
    sim::TypeRegistry standard, crusades;
    sim::setupRegistry(standard, base, false);
    sim::setupRegistry(crusades, base, true);
    auto vfs = hpi::mountRetailRoot(root);
    int hits = 0, scenarios = 0;
    for (const auto& path : vfs.list("kmap")) {
        if (!path.ends_with(".tnt")) continue;
        const auto crtBytes = vfs.tryRead(path.substr(0, path.size() - 4) + ".crt");
        if (!crtBytes) continue;
        const auto scenario = crt::parse(*crtBytes);
        if (scenario.version != 1) continue;
        ++scenarios;
        const auto issues = cart::validateCrusadesRoster(scenario, cart::toPlaced(scenario, &standard), standard, crusades);
        if (issues.empty()) continue;
        ++hits;
        const auto onStd = sim::inspectScenario(vfs, path, &standard), onCb = sim::inspectScenario(vfs, path, &crusades);
        std::printf("%s: standard %s, crusades %s%s%s\n", path.c_str(), sim::scenarioReason(onStd).empty() ? "offered" : sim::scenarioReason(onStd)[0].c_str(),
                    onCb.status == sim::ScenarioStatus::Ok ? "offered" : "refused", onCb.detail.empty() ? "" : " -- needs ",
                    onCb.detail.c_str());
        for (const auto& issue : issues) std::printf("    %s\n", issue.message.c_str());
    }
    std::printf("%d of %d scenario maps lose units with CRUSADES on\n", hits, scenarios);
    return 0;
}

int main(int argc, char** argv) try {
    if (argc < 2) { std::fprintf(stderr, "usage: crusades_scenario_test <install> [--survey <root>]\n"); return 2; }
    if (argc == 4 && !std::strcmp(argv[2], "--survey")) return survey(argv[1], argv[3]);
    auto data = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry standard, crusades;
    sim::setupRegistry(standard, data, false);
    sim::setupRegistry(crusades, data, true);
    check(standard.find("zonspide") && !crusades.find("zonspide") && crusades.find("araking"),
          "fixture needs an install whose Crusades roster leaves out zonspide");

    // A copy of Ulasem Arena whose scenario places a Monarch for each seat.
    const auto stock = net::maps::build(data, "Ulasem Arena");
    check(stock != nullptr, "fixture needs Ulasem Arena");
    const auto starts = sim::parseStartPositions(data, stock->mapPath);
    check(starts.size() >= 2, "fixture needs two starts");
    crt::Scenario scenario;
    scenario.players.resize(9);
    for (int p = 0; p < 2; ++p) {
        crt::Unit monarch;
        monarch.objectName = "ARAKING";
        monarch.player = p;
        monarch.x = int(starts[size_t(p)].first / 16);
        monarch.z = int(starts[size_t(p)].second / 16);
        scenario.units.push_back(monarch);
    }
    auto files = std::make_shared<hpi::Vfs::Files>();
    const auto stem = stock->mapPath.substr(0, stock->mapPath.size() - 4);
    for (const auto& [name, bytes] : *stock->files) {
        if (!name.starts_with(stem + ".")) continue;
        const auto path = "kmap/crusades scenario" + name.substr(stem.size());
        if (path.ends_with(".ota")) {
            auto metadata = tnt::Scenario::parse(std::string(bytes.begin(), bytes.end()));
            metadata.hasScenario = true;
            const auto text = metadata.write();
            (*files)[path] = {text.begin(), text.end()};
        } else if (!path.ends_with(".crt")) (*files)[path] = bytes;
    }
    hpi::Vfs view(&data);
    view.setMapFiles(files);
    sim::MatchConfig cfg;
    cfg.vfs = &view;
    cfg.mapPath = "kmap/crusades scenario.tnt";
    cfg.mapScenario = true;
    cfg.slots = {{true, 0, 0}, {true, 0, 1}};
    auto play = [&](const crt::Scenario& s, sim::World& world, const sim::TypeRegistry& reg) {
        (*files)["kmap/crusades scenario.crt"] = crt::write(s);
        view.setMapFiles(files);
        sim::setupMatch(world, reg, cfg);
    };

    {   // (a) Custom stats for a unit Crusades lacks are skipped, as retail skips them.
        auto custom = scenario;
        custom.customTypes.push_back({"ZONSPIDE", {100, 200, 200, 0}});
        custom.customTypes.push_back({"ARAKING", {100, 150, 250, 2}});
        sim::World world;
        std::string refused;
        try { play(custom, world, crusades); } catch (const std::exception& e) { refused = e.what(); }
        check(refused.empty(), "a Crusades scenario with a custom type Crusades lacks did not start: " + refused);
        check(world.units().size() == 2 && world.scenario(), "the Crusades scenario lost its placements");
        const auto& monarch = world.units()[0];
        check(monarch.scenarioArmor > 1.49f && monarch.scenarioArmor < 1.51f &&
              monarch.scenarioWeapon > 2.49f && monarch.scenarioWeapon < 2.51f,
              "the known custom type lost its stats beside the skipped one");
        check(sim::inspectScenario(view, cfg.mapPath, &crusades).status == sim::ScenarioStatus::Ok,
              "the CHANGE MAP panel does not offer a scenario whose only loss is a custom type");
    }
    {   // (b) A placed unit Crusades lacks is refused, by name, for the player.
        auto placed = scenario;
        placed.units[1].objectName = "ZONSPIDE";
        sim::World world;
        std::string text;
        try { play(placed, world, crusades); } catch (const sim::ScenarioTypeMissing& e) {
            check(e.type == "ZONSPIDE", "the refusal lost the CRT's type name");
            text = sim::scenarioTypeMissingText(e, &standard);
            check(sim::scenarioTypeMissingText(e, nullptr) ==
                      "this scenario places ZONSPIDE, a unit this game's data does not have",
                  "the standard-balance refusal changed");
        }
        const auto name = standard.find("zonspide")->name;
        const auto want = "this scenario's " + name + " (ZONSPIDE) does not exist with the CRUSADES balance on"
                          " -- turn CRUSADES off to play it";
        check(text == want, "Crusades refusal text: got '" + text + "', want '" + want + "'");
        sim::World plain;
        play(placed, plain, standard);
        check(plain.units().size() == 2, "the standard balance no longer plays the scenario");
        std::printf("refusal: %s\n", text.c_str());
    }
    {   // (c) Check Map warns on the real rosters, and nothing for the standard one.
        auto placed = scenario;
        placed.units[1].objectName = "ZONSPIDE";
        const auto units = cart::toPlaced(placed, &standard);
        const auto issues = cart::validateCrusadesRoster(placed, units, standard, crusades);
        check(issues.size() == 1 && issues[0].message.find("(ZONSPIDE) does not exist with CRUSADES on") != std::string::npos,
              "Check Map did not name the placed zonspide");
        std::printf("check map: %s\n", issues[0].message.c_str());
        check(cart::validateCrusadesRoster(scenario, cart::toPlaced(scenario, &standard), standard, crusades).empty(),
              "Check Map warned about a scenario Crusades plays whole");
    }
    {   // (d) A Test Map copy's Use Only entry that only the other balance has permits
        // nothing under CRUSADES; a name neither balance has is still the author's typo.
        const auto ota = "kmap/crusades scenario.ota";
        auto text = std::string((*files)[ota].begin(), (*files)[ota].end());
        auto metadata = tnt::Scenario::parse(text);
        metadata.useOnlyUnits = "crusades scenario.tdf";
        text = metadata.write() + "\n[TAKPlaytest]\n{\nauthoredscenario=1;\n}\n";
        (*files)[ota] = {text.begin(), text.end()};
        auto useOnly = [&](const char* list) {
            const std::string tdf = list;
            (*files)["kmap/crusades scenario.tdf"] = {tdf.begin(), tdf.end()};
        };
        useOnly("[ARAKING]\n{\n}\n[ZONSPIDE]\n{\n}\n");
        sim::World world;
        std::string refused;
        try { play(scenario, world, crusades); } catch (const std::exception& e) { refused = e.what(); }
        check(refused.empty(), "a Crusades Test Map refused a Use Only unit of the standard balance: " + refused);
        check(world.units().size() == 2, "the Crusades Test Map lost its placements");
        check(sim::inspectScenario(view, cfg.mapPath, &crusades).status == sim::ScenarioStatus::Ok,
              "the CHANGE MAP panel does not offer a Crusades Test Map whose Use Only names zonspide");
        useOnly("[ARAKING]\n{\n}\n[ZONSPIDX]\n{\n}\n");
        sim::World typo;
        refused.clear();
        try { play(scenario, typo, crusades); } catch (const std::exception& e) { refused = e.what(); }
        check(refused.find("Unknown authored Use Only unit: zonspidx") != std::string::npos,
              "a Test Map's Use Only typo was accepted: " + refused);
        check(sim::inspectScenario(view, cfg.mapPath, &crusades).status == sim::ScenarioStatus::MissingType,
              "the CHANGE MAP panel offers a Test Map with a Use Only typo");
    }
    std::printf("PASS: Crusades scenario custom types skipped, placed types refused by name, Check Map warns\n");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
