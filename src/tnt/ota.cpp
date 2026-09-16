#include "tnt/ota.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "tdf/tdf.h"

#include <cstdio>

namespace ta::tnt {

Scenario Scenario::parse(const std::string& text) {
    Scenario s;
    ta::tdf::Node root = ta::tdf::parseText(text, "<ota>");
    const ta::tdf::Node* gh = root.child("globalheader");
    if (!gh) return s;
    s.copyright = gh->valueOr("copyright", s.copyright);
    s.missionName = gh->valueOr("missionname", "");
    s.missionDescription = gh->valueOr("missiondescription", "");
    s.kingdom = gh->valueOr("kingdom", "");
    s.useOnlyUnits = gh->valueOr("useonlyunits", "");
    s.hasScenario = gh->numberOr("hasscenario", 0) != 0;
    s.tidalStrength = float(gh->numberOr("tidalstrength", 0));
    s.solarStrength = float(gh->numberOr("solarstrength", 0));
    s.minWindSpeed = float(gh->numberOr("minwindspeed", 0));
    s.maxWindSpeed = float(gh->numberOr("maxwindspeed", 0));
    s.gravity = float(gh->numberOr("gravity", 0));
    s.lineOfSight = int(gh->numberOr("lineofsight", 0));
    // size = "W x H" (Units)
    if (const std::string* sz = gh->value("size"))
        std::sscanf(sz->c_str(), "%d x %d", &s.sizeW, &s.sizeH);
    // TA nests the per-game-type block as [Schema N] inside [GlobalHeader], and
    // the start positions inside THAT: [GlobalHeader][Schema N][specials]
    // [specialN]{ specialwhat=StartPosK; XPos; ZPos }. (Kingdoms used a single
    // [Map Data] section, which is what this looked for -- so a retail .ota
    // yielded no start positions, no metal richness and no AI profile at all.)
    //
    // A map may declare several schemas, one per player count. Take the first
    // that carries specials; the lobby has no schema picker, and every shipped
    // map's schemas agree on the economy figures.
    // --- campaign mission metadata -------------------------------------------
    s.planet = gh->valueOr("planet", "");
    s.missionHint = gh->valueOr("missionhint", "");
    s.brief = gh->valueOr("brief", "");
    s.narration = gh->valueOr("narration", "");
    s.glamour = gh->valueOr("glamour", "");
    s.lavaWorld = gh->numberOr("lavaworld", 0) != 0;
    s.killMul = int(gh->numberOr("killmul", 0));
    s.timeMul = int(gh->numberOr("timemul", 0));
    s.maxUnits = int(gh->numberOr("maxunits", 0));
    s.waterDoesDamage = gh->numberOr("waterdoesdamage", 0) != 0;
    s.waterDamage = float(gh->numberOr("waterdamage", 0));
    s.noSeaLevelTrigger = gh->numberOr("nosealeveltrigger", 0) != 0;
    s.mapping = int(gh->numberOr("mapping", 0));

    // --- objectives ----------------------------------------------------------
    // Plain header keys. A boolean one is "declared and non-zero"; a typed one
    // names the unit it is about.
    {
        Objectives& o = s.objectives;
        o.allUnitsKilled = gh->numberOr("allunitskilled", 0) != 0;
        o.commanderKilled = gh->numberOr("commanderkilled", 0) != 0;
        o.destroyAllUnits = gh->numberOr("destroyallunits", 0) != 0;
        o.killAllMobileUnits = gh->numberOr("killallmobileunits", 0) != 0;
        o.deathTimerRunsOut = gh->numberOr("deathtimerrunsout", 0) != 0;
        o.allUnitsKilledOfType = gh->valueOr("allunitskilledoftype", "");
        o.killAllOfType = gh->valueOr("killalloftype", "");
        o.killUnitType = gh->valueOr("killunittype", "");
        o.captureUnitType = gh->valueOr("captureunittype", "");
        o.unitTypeKilled = gh->valueOr("unittypekilled", "");
        o.buildUnitType = gh->valueOr("buildunittype", "");
        // MoveUnitToRadius=<type>, x, z, radius -- one key, four comma-separated
        // fields. AC01's reads "ANYTYPE, 992, 656, 64".
        const std::string mv = gh->valueOr("moveunittoradius", "");
        if (!mv.empty()) {
            std::vector<std::string> f;
            std::string cur;
            for (char c : mv) {
                if (c == ',') { f.push_back(cur); cur.clear(); }
                else cur += c;
            }
            f.push_back(cur);
            auto trim = [](std::string v) {
                size_t a = v.find_first_not_of(" \t");
                size_t b = v.find_last_not_of(" \t");
                return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
            };
            if (f.size() >= 4) {
                o.hasMoveToRadius = true;
                o.moveToType = trim(f[0]);
                o.moveToX = std::atoi(trim(f[1]).c_str());
                o.moveToZ = std::atoi(trim(f[2]).c_str());
                o.moveToRadius = std::atoi(trim(f[3]).c_str());
            }
        }
    }

    // --- every schema, in file order -----------------------------------------
    const ta::tdf::Node* md = nullptr;
    for (const std::string& nm : gh->childOrder) {
        const ta::tdf::Node* cand = gh->child(nm);
        if (!cand) continue;
        std::string lo = nm;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        if (lo.rfind("schema", 0) != 0) continue;

        Schema sc;
        sc.type = cand->valueOr("type", "");
        sc.aiProfile = cand->valueOr("aiprofile", "");
        sc.surfaceMetal = float(cand->numberOr("surfacemetal", 0));
        sc.mohoMetal = float(cand->numberOr("mohometal", 0));
        sc.humanMetal = float(cand->numberOr("humanmetal", 0));
        sc.humanEnergy = float(cand->numberOr("humanenergy", 0));
        sc.computerMetal = float(cand->numberOr("computermetal", 0));
        sc.computerEnergy = float(cand->numberOr("computerenergy", 0));
        if (const ta::tdf::Node* un = cand->child("units")) {
            for (const std::string& un2 : un->childOrder) {
                const ta::tdf::Node* u = un->child(un2);
                if (!u) continue;
                PlacedUnit pu;
                pu.unitName = u->valueOr("unitname", "");
                if (pu.unitName.empty()) continue;
                pu.ident = u->valueOr("ident", "");
                pu.xpos = int(u->numberOr("xpos", 0));
                pu.ypos = int(u->numberOr("ypos", 0));
                pu.zpos = int(u->numberOr("zpos", 0));
                pu.player = int(u->numberOr("player", 0));
                pu.healthPercent = int(u->numberOr("healthpercentage", 100));
                pu.angle = int(u->numberOr("angle", 0));
                pu.kills = int(u->numberOr("kills", 0));
                sc.units.push_back(std::move(pu));
            }
        }
        if (const ta::tdf::Node* sp = cand->child("specials")) {
            for (const std::string& nm2 : sp->childOrder) {
                const ta::tdf::Node* one = sp->child(nm2);
                if (!one) continue;
                std::string what = one->valueOr("specialwhat", "");
                if (what.rfind("StartPos", 0) != 0 && what.rfind("startpos", 0) != 0)
                    continue;
                StartPos sp2;
                sp2.number = std::atoi(what.c_str() + 8);
                sp2.xpos = int(one->numberOr("xpos", 0));
                sp2.zpos = int(one->numberOr("zpos", 0));
                sc.starts.push_back(sp2);
            }
            std::sort(sc.starts.begin(), sc.starts.end(),
                      [](const StartPos& a, const StartPos& b) { return a.number < b.number; });
        }
        s.schemas.push_back(std::move(sc));

        if (!md) md = cand;                    // first schema: the fallback
        if (cand->child("specials") && !md->child("specials")) md = cand;
    }
    if (md) {
        s.mapType = md->valueOr("type", s.mapType);
        s.aiProfile = md->valueOr("aiprofile", s.aiProfile);
        // Metal richness is a per-SCHEMA property: the same map can offer a
        // different economy per game type.
        s.surfaceMetal = float(md->numberOr("surfacemetal", 0));
        s.mohoMetal = float(md->numberOr("mohometal", 0));
        if (const ta::tdf::Node* sp = md->child("specials")) {
            // [special0], [special1], ... each with specialwhat/XPos/ZPos.
            for (const std::string& nm : sp->childOrder) {
                const ta::tdf::Node* one = sp->child(nm);
                if (!one) continue;
                std::string what = one->valueOr("specialwhat", "");
                if (what.rfind("StartPos", 0) != 0 && what.rfind("startpos", 0) != 0)
                    continue;
                StartPos p;
                p.number = std::atoi(what.c_str() + 8);
                // World units, verbatim -- see StartPos in ota.h.
                p.xpos = int(one->numberOr("xpos", 0));
                p.zpos = int(one->numberOr("zpos", 0));
                s.starts.push_back(p);
            }
        }
    }
    return s;
}

std::string Scenario::write() const {
    std::string o;
    auto line = [&](int depth, const std::string& text) {
        o.append(size_t(depth), '\t');
        o += text;
        o += "\r\n";
    };
    // A [Section] header sits at depth D; its brace + body sit at D+1.
    line(0, "[GlobalHeader]");
    line(1, "{");
    line(1, "Copyright=" + copyright + ";");
    line(1, "missionname=" + missionName + ";");
    line(1, "missiondescription=" + missionDescription + ";");
    line(1, "kingdom=" + kingdom + ";");
    line(1, "numplayers=" + std::to_string(starts.size()) + ";");
    line(1, "size=" + std::to_string(sizeW) + " x " + std::to_string(sizeH) + ";");
    line(1, "memory=32 MB;");
    if (!useOnlyUnits.empty()) line(1, "useonlyunits=" + useOnlyUnits + ";");
    line(1, std::string("hasscenario=") + (hasScenario ? "1" : "0") + ";");
    // The economy the map advertises. These were parsed but never written, so a
    // map this editor saved came back with no wind, no tide and no gravity -- and
    // a wind generator on it earned exactly nothing.
    line(1, "tidalstrength=" + std::to_string(int(tidalStrength)) + ";");
    line(1, "solarstrength=" + std::to_string(int(solarStrength)) + ";");
    line(1, "minwindspeed=" + std::to_string(int(minWindSpeed)) + ";");
    line(1, "maxwindspeed=" + std::to_string(int(maxWindSpeed)) + ";");
    line(1, "gravity=" + std::to_string(int(gravity)) + ";");
    // [Schema 0], as TA writes it -- not Kingdoms' [Map Data]. The economy
    // figures belong to the schema, not the header.
    line(1, "[Schema 0]");
    line(2, "{");
    line(2, "Type=" + mapType + ";");
    line(2, "aiprofile=" + aiProfile + ";");
    line(2, "SurfaceMetal=" + std::to_string(int(surfaceMetal)) + ";");
    line(2, "MohoMetal=" + std::to_string(int(mohoMetal)) + ";");
    line(2, "[specials]");
    line(3, "{");
    for (size_t i = 0; i < starts.size(); ++i) {
        const StartPos& p = starts[i];
        line(3, "[special" + std::to_string(i) + "]");
        line(4, "{");
        line(4, "specialwhat=StartPos" + std::to_string(p.number) + ";");
        line(4, "XPos=" + std::to_string(p.xpos) + ";");
        line(4, "ZPos=" + std::to_string(p.zpos) + ";");
        line(4, "}");
    }
    line(3, "}");   // specials
    line(2, "}");   // Schema 0
    line(1, "}");   // GlobalHeader
    return o;
}

const Schema* Scenario::schemaFor(const std::string& type) const {
    if (schemas.empty()) return nullptr;
    if (!type.empty()) {
        for (const auto& sc : schemas) {
            if (sc.type.size() != type.size()) continue;
            bool same = true;
            for (size_t i = 0; i < sc.type.size(); ++i)
                if (std::tolower(uint8_t(sc.type[i])) != std::tolower(uint8_t(type[i])))
                    { same = false; break; }
            if (same) return &sc;
        }
    }
    return &schemas.front();
}

} // namespace ta::tnt
