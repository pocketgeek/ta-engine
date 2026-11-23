#include "sim/scenarioinfo.h"

#include "crt/crt.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "tdf/tdf.h"
#include "util/virtualpath.h"

#include <algorithm>
#include <cctype>
#include <exception>

namespace tak::sim {

namespace {
std::string upper(std::string s, size_t limit) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (s.size() > limit) s.resize(limit);
    return s;
}
int seatCount(uint8_t seats) {
    int n = 0;
    for (; seats; seats &= uint8_t(seats - 1)) ++n;
    return n;
}
}  // namespace

ScenarioInfo inspectScenario(const hpi::Vfs& vfs, const std::string& mapPath, const TypeRegistry* reg) {
    ScenarioInfo info;
    try {
        const auto otaPath = tak::vpath::replaceExtension(mapPath, ".ota");
        const auto otaBytes = vfs.tryRead(otaPath);
        if (!otaBytes) return info;
        const auto root = tdf::parseText(std::string(otaBytes->begin(), otaBytes->end()), otaPath);
        if (const auto* test = root.child("takplaytest")) info.marker = test->numberOr("authoredscenario", 0) != 0;
        const auto* gh = root.child("globalheader");
        info.hasScenario = gh && gh->numberOr("hasscenario", 0) != 0;

        // The Use Only list, by setupMatch's rules: a plain file name, the map's own
        // <map>.tdf beside the .ota, present. A Test Map copy's entries (by section name)
        // must all be known types; a MAP SCENARIO game leaves out a name no unit answers to.
        const std::string restriction = gh ? gh->valueOr("useonlyunits", "") : std::string();
        bool useOnlyBroken = false;
        std::string missingType;
        if (!restriction.empty()) {
            const auto slash = otaPath.find_last_of('/');
            const auto path = (slash == std::string::npos ? std::string{} : otaPath.substr(0, slash + 1)) + restriction;
            if (tak::vpath::filename(restriction) != restriction || restriction == "." || restriction == ".." ||
                hpi::MountSet::key(path) != hpi::MountSet::key(tak::vpath::replaceExtension(otaPath, ".tdf"))) {
                useOnlyBroken = true;
            } else if (!vfs.has(path)) {
                useOnlyBroken = info.useOnlyMissing = true;
            } else {
                try {
                    const auto data = vfs.read(path);
                    const auto whitelist = tdf::parseText(std::string(data.begin(), data.end()), path);
                    int known = 0;
                    for (const auto& name : whitelist.childOrder) {
                        if (!reg || reg->find(name)) { ++known; continue; }
                        if (info.marker && missingType.empty() && !balanceUnitFile(vfs, name)) missingType = name;
                    }
                    info.useOnlyTypes = info.marker ? int(whitelist.childOrder.size()) : known;
                } catch (const std::exception&) { useOnlyBroken = true; }
            }
            if (useOnlyBroken) info.useOnlyTypes = -1;
        }

        const auto crtBytes = vfs.tryRead(tak::vpath::replaceExtension(mapPath, ".crt"));
        crt::Scenario scenario;
        if (crtBytes) scenario = crt::parse(*crtBytes);
        info.crtValid = crtBytes && scenario.version == 1;
        bool badOwner = false;
        if (info.crtValid) {
            info.placed = int(scenario.units.size());
            for (const auto& unit : scenario.units) {
                if (unit.player >= 0 && unit.player < kMaxPlayers) info.placedSeats |= uint8_t(1u << unit.player);
                else if (unit.player != kMaxPlayers) badOwner = true;   // 8 is the neutral owner
            }
            // Bank 0 is All Players, banks 1..8 belong to owners 0..7 (setupMatch's seat
            // rule). A Cartographer CRT has exactly nine; a later bank would name the
            // neutral owner (no seat) or no owner at all, which setupMatch refuses.
            for (size_t p = 0; p < scenario.players.size(); ++p) {
                const auto& groups = scenario.players[p];
                const bool rules = std::any_of(groups.begin(), groups.end(), crt::hasRules);
                if (p == 0) info.sharedRules = rules;
                else if (p <= size_t(kMaxPlayers)) { if (rules) info.ruleSeats |= uint8_t(1u << (p - 1)); }
                else if (p > size_t(kMaxPlayers) + 1 && !groups.empty()) badOwner = true;
                for (const auto& group : groups)
                    for (const auto& action : group.actions)
                        if (crt::isOutcomeAction(action.opcode)) info.outcomes = true;
            }
            auto lower = [](std::string s) {
                for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            // setupMatch's order: Use Only entries, then placements. A custom type no unit
            // answers to is skipped there, as retail does, so it is not a missing type.
            if (reg && missingType.empty())
                for (const auto& unit : scenario.units)
                    if (!reg->find(lower(unit.objectName))) { missingType = unit.objectName; break; }
            // Who has something to command once the placements replace the Monarchs. A
            // Create action (7) makes units for its bank's owner, or for every player
            // from All Players (bank 0).
            if (reg) {
                info.armySeats = 0;
                auto creates = [&](size_t bank) {
                    if (bank >= scenario.players.size()) return false;
                    for (const auto& group : scenario.players[bank])
                        for (const auto& action : group.actions)
                            if (action.opcode == 7) return true;
                    return false;
                };
                const bool everyone = creates(0);
                for (int p = 0; p < kMaxPlayers; ++p)
                    if (everyone || creates(size_t(p) + 1)) info.armySeats |= uint8_t(1u << p);
                for (const auto& unit : scenario.units) {
                    if (unit.player < 0 || unit.player >= kMaxPlayers) continue;
                    // A type this install lacks may come from a game mod pack (MissingType
                    // already refuses or warns): it is not held against the seat.
                    const auto* type = reg->find(lower(unit.objectName));
                    if (!type) { info.armySeats |= uint8_t(1u << unit.player); continue; }
                    const auto side = lower(type->side);
                    if (side == "lif" || side == "npc" || side == "mon") continue;
                    bool armed = false;
                    for (const auto& weapon : type->weapons) armed = armed || weapon.damage > 0;
                    if (armed || type->isBuilder || !type->isStructure()) info.armySeats |= uint8_t(1u << unit.player);
                }
            }
        }

        using S = ScenarioStatus;
        if (!info.hasScenario) info.status = info.content() ? S::Leftover : S::NoScenario;
        else if (!crtBytes) info.status = S::CrtMissing;
        else if (!info.crtValid) info.status = S::CrtDamaged;
        else if (!info.content()) info.status = S::Empty;
        else if (badOwner) info.status = S::BadOwner;
        else if (useOnlyBroken) { info.status = S::UseOnlyBroken; info.detail = restriction; }
        else if (!missingType.empty()) { info.status = S::MissingType; info.detail = missingType; }
        else info.status = S::Ok;
    } catch (const std::exception&) {
        // An .ota that does not parse plays no scenario (setupMatch cannot read it either).
        const bool marker = info.marker;
        info = ScenarioInfo{};
        info.marker = marker;
    }
    return info;
}

std::string seatList(uint8_t seats) {
    std::string out;
    for (int i = 0; i < kMaxPlayers;) {
        if (!(seats >> i & 1)) { ++i; continue; }
        int end = i;
        while (end + 1 < kMaxPlayers && (seats >> (end + 1) & 1)) ++end;
        auto add = [&](const std::string& part) { if (!out.empty()) out += ' '; out += part; };
        if (end - i >= 2) add(std::to_string(i + 1) + "-" + std::to_string(end + 1));
        else for (int k = i; k <= end; ++k) add(std::to_string(k + 1));
        i = end + 1;
    }
    return out;
}

std::vector<std::string> scenarioReason(const ScenarioInfo& info) {
    using S = ScenarioStatus;
    switch (info.status) {
        case S::Ok:
            if (info.seatOneArmy()) return {};
            return {"PLAYER 1 GETS NO ARMY", info.placedSeats & 1 ? "(NO FIGHTER OR BUILDER)" : "(NO UNITS PLACED FOR IT)"};
        case S::NoScenario: return {"THIS MAP HAS NO SCENARIO"};
        case S::Leftover: return {"THIS MAP HAS NO SCENARIO", "(UNUSED LEFTOVER RULES)"};
        case S::CrtMissing: return {"SCENARIO FILE IS MISSING"};
        case S::CrtDamaged: return {"SCENARIO FILE IS DAMAGED"};
        case S::Empty: return {"SCENARIO IS EMPTY"};
        case S::BadOwner: return {"SCENARIO OWNER IS INVALID"};
        case S::UseOnlyBroken: return {"USE ONLY LIST IS BROKEN", upper(info.detail, 26)};
        case S::MissingType: return {"NEEDS A UNIT NOT INSTALLED", upper(info.detail, 26)};
    }
    return {};
}

std::vector<ScenarioLine> scenarioSummary(const ScenarioInfo& info, bool compact) {
    std::vector<ScenarioLine> lines;
    if (info.placed > 0) {
        if (!compact) lines.push_back({"PLACED UNITS REPLACE MONARCHS"});
        else if (!info.placedSeats) lines.push_back({"NEUTRAL UNITS ONLY"});
        else {
            std::string text = "UNITS PLACED FOR " + seatList(info.placedSeats);
            // A scattered seat set can outgrow the column; say how many instead.
            if (text.size() > 26) text = "UNITS PLACED FOR " + std::to_string(seatCount(info.placedSeats)) + " SEATS";
            lines.push_back({text});
        }
    } else {
        lines.push_back({compact ? "RULES ONLY: MONARCHS STAY" : "RULES ONLY: EVERYONE KEEPS A MONARCH"});
    }
    const uint8_t seats = info.requiredSeats();
    lines.push_back({seats ? "NEEDS SEATS " + seatList(seats) : std::string("NEEDS NO SEATS")});
    if (auto useOnly = scenarioUseOnlyLine(info, compact); !useOnly.empty()) lines.push_back({std::move(useOnly)});
    if (info.outcomes) lines.push_back({compact ? "ENDS BY SCENARIO RULES" : "ENDS ONLY BY THE SCENARIO'S RULES"});
    else lines.push_back({compact ? "NO VICTORY RULES: ENDLESS" : "NO VICTORY RULES: IT NEVER ENDS BY ITSELF", true});
    return lines;
}

std::string scenarioUseOnlyLine(const ScenarioInfo& info, bool compact) {
    if (info.useOnlyTypes > 0) {
        const std::string types = std::to_string(info.useOnlyTypes) + (info.useOnlyTypes == 1 ? " UNIT TYPE" : " UNIT TYPES");
        return compact ? "BUILDS " + types : "ONLY " + types + " CAN BE BUILT";
    }
    if (info.useOnlyTypes == 0) return compact ? "NOTHING CAN BE BUILT" : "NO UNIT TYPE CAN BE BUILT";
    return compact ? std::string() : std::string("ALL UNIT TYPES CAN BE BUILT");
}

}  // namespace tak::sim
