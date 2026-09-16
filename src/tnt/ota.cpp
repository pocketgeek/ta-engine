#include "tnt/ota.h"

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
    const ta::tdf::Node* md = gh->child("map data");
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
    line(1, "[Map Data]");
    line(2, "{");
    line(2, "Type=" + mapType + ";");
    line(2, "aiprofile=" + aiProfile + ";");
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
    line(2, "}");   // Map Data
    line(1, "}");   // GlobalHeader
    return o;
}

} // namespace ta::tnt
