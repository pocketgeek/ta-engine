#include "tnt/ota.h"

#include "tdf/tdf.h"

#include <cstdio>

namespace ta::tnt {

namespace {
// OTA assignments are single lines, not quoted/escaped strings. Keep a readable
// legacy value and preserve text that cannot be represented there in an optional
// engine extension. Do not change how ordinary retail TDF values are parsed.
std::string legacyText(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (c < 32 || c == 127 || c == '{' || c == '}') out += ' ';
        else if (c == '/' && !out.empty() && out.back() == '/') out += " /";
        else out += char(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == ';')) out.pop_back();
    auto first = out.find_first_not_of(' ');
    return first == std::string::npos ? std::string{} : out.substr(first);
}
std::string hexText(const std::string& text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : text) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}
void restoreText(const ta::tdf::Node* node, const char* key, std::string& text) {
    const auto* encoded = node ? node->value(key) : nullptr;
    if (!encoded || encoded->size() % 2 || encoded->size() > 131072) return;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string decoded;
    for (size_t i = 0; i < encoded->size(); i += 2) {
        int a = digit((*encoded)[i]), b = digit((*encoded)[i + 1]);
        if (a < 0 || b < 0) return;
        decoded += char((a << 4) | b);
    }
    text = std::move(decoded);
}
} // namespace

Scenario Scenario::parse(const std::string& text) {
    Scenario s;
    ta::tdf::Node root = ta::tdf::parseText(text, "<ota>");
    const ta::tdf::Node* gh = root.child("globalheader");
    if (!gh) return s;
    s.copyright = gh->valueOr("copyright", s.copyright);
    s.missionName = gh->valueOr("missionname", "");
    s.missionDescription = gh->valueOr("missiondescription", "");
    s.generatorRecipe = gh->valueOr("tageneratorrecipe", "");
    s.kingdom = gh->valueOr("planet", gh->valueOr("kingdom", ""));
    s.tidalStrength = int(gh->numberOr("tidalstrength", s.tidalStrength));
    s.solarStrength = int(gh->numberOr("solarstrength", s.solarStrength));
    s.minWindSpeed = int(gh->numberOr("minwindspeed", s.minWindSpeed));
    s.maxWindSpeed = int(gh->numberOr("maxwindspeed", s.maxWindSpeed));
    s.gravity = int(gh->numberOr("gravity", s.gravity));
    s.lavaWorld = int(gh->numberOr("lavaworld", s.lavaWorld));
    s.killMul = int(gh->numberOr("killmul", s.killMul));
    s.useOnlyUnits = gh->valueOr("useonlyunits", "");
    s.hasScenario = gh->numberOr("hasscenario", 0) != 0;
    // size = "W x H" (Units)
    if (const std::string* sz = gh->value("size"))
        std::sscanf(sz->c_str(), "%d x %d", &s.sizeW, &s.sizeH);
    // Total Annihilation: [Schema 0] carries the type, aiprofile, metal/energy and
    // the start specials in PIXELS (converted to cells here).
    if (const ta::tdf::Node* sc = gh->child("schema 0")) {
        s.mapType = sc->valueOr("type", s.mapType);
        s.aiProfile = sc->valueOr("aiprofile", s.aiProfile);
        s.surfaceMetal = int(sc->numberOr("surfacemetal", s.surfaceMetal));
        s.mohoMetal = int(sc->numberOr("mohometal", s.mohoMetal));
        s.humanMetal = int(sc->numberOr("humanmetal", s.humanMetal));
        s.computerMetal = int(sc->numberOr("computermetal", s.computerMetal));
        s.humanEnergy = int(sc->numberOr("humanenergy", s.humanEnergy));
        s.computerEnergy = int(sc->numberOr("computerenergy", s.computerEnergy));
        if (const ta::tdf::Node* sp = sc->child("specials"))
            for (const std::string& nm : sp->childOrder) {
                const ta::tdf::Node* one = sp->child(nm);
                if (!one) continue;
                std::string what = one->valueOr("specialwhat", "");
                if (what.rfind("StartPos", 0) != 0 && what.rfind("startpos", 0) != 0) continue;
                StartPos p;
                p.number = std::atoi(what.c_str() + 8);
                p.xpos = int(one->numberOr("xpos", 0)) / 16;
                p.zpos = int(one->numberOr("zpos", 0)) / 16;
                s.starts.push_back(p);
            }
    }
    const ta::tdf::Node* md = gh->child("map data");
    if (md && s.starts.empty()) {
        s.mapType = md->valueOr("type", s.mapType);
        s.aiProfile = md->valueOr("aiprofile", s.aiProfile);
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
    const auto* textFields = root.child("taktext");
    restoreText(textFields, "copyright", s.copyright);
    restoreText(textFields, "missionname", s.missionName);
    restoreText(textFields, "missiondescription", s.missionDescription);
    restoreText(textFields, "tageneratorrecipe", s.generatorRecipe);
    restoreText(textFields, "kingdom", s.kingdom);
    restoreText(textFields, "useonlyunits", s.useOnlyUnits);
    restoreText(textFields, "type", s.mapType);
    restoreText(textFields, "aiprofile", s.aiProfile);
    return s;
}

std::string Scenario::write() const {
    std::string o;
    auto line = [&](int depth, const std::string& text) {
        o.append(size_t(depth), '\t');
        o += text;
        o += "\r\n";
    };
    // Total Annihilation's .ota layout (as the shipped maps): [GlobalHeader] with
    // the world and environment, then [Schema 0] with type, AI profile, resources
    // and the start specials in PIXELS.
    line(0, "[GlobalHeader]");
    line(1, "{");
    line(1, "missionname=" + legacyText(missionName) + ";");
    line(1, "missiondescription=" + legacyText(missionDescription) + ";");
    if(!generatorRecipe.empty())line(1, "tageneratorrecipe=" + legacyText(generatorRecipe) + ";");
    line(1, "planet=" + legacyText(kingdom) + ";");
    line(1, "missionhint=;");
    line(1, "brief=;");
    line(1, "narration=;");
    line(1, "glamour=;");
    line(1, "lineofsight=0;");
    line(1, "mapping=0;");
    line(1, "tidalstrength=" + std::to_string(tidalStrength) + ";");
    line(1, "solarstrength=" + std::to_string(solarStrength) + ";");
    line(1, "lavaworld=" + std::to_string(lavaWorld) + ";");
    line(1, "killmul=" + std::to_string(killMul) + ";");
    line(1, "timemul=0;");
    line(1, "minwindspeed=" + std::to_string(minWindSpeed) + ";");
    line(1, "maxwindspeed=" + std::to_string(maxWindSpeed) + ";");
    line(1, "gravity=" + std::to_string(gravity) + ";");
    line(1, "numplayers=" + std::to_string(starts.size()) + ";");
    line(1, "size=" + std::to_string(sizeW) + " x " + std::to_string(sizeH) + ";");
    line(1, "memory=32 MB;");
    if (!useOnlyUnits.empty()) line(1, "useonlyunits=" + legacyText(useOnlyUnits) + ";");
    if (hasScenario) line(1, "hasscenario=1;");
    line(1, "SCHEMACOUNT=1;");
    line(1, "[Schema 0]");
    line(2, "{");
    line(2, "Type=" + legacyText(mapType) + ";");
    line(2, "aiprofile=" + legacyText(aiProfile) + ";");
    line(2, "SurfaceMetal=" + std::to_string(surfaceMetal) + ";");
    line(2, "MohoMetal=" + std::to_string(mohoMetal) + ";");
    line(2, "HumanMetal=" + std::to_string(humanMetal) + ";");
    line(2, "ComputerMetal=" + std::to_string(computerMetal) + ";");
    line(2, "HumanEnergy=" + std::to_string(humanEnergy) + ";");
    line(2, "ComputerEnergy=" + std::to_string(computerEnergy) + ";");
    line(2, "MeteorWeapon=;");
    line(2, "MeteorRadius=0;");
    line(2, "MeteorDensity=0;");
    line(2, "MeteorDuration=0;");
    line(2, "MeteorInterval=0;");
    line(2, "[specials]");
    line(3, "{");
    for (size_t i = 0; i < starts.size(); ++i) {
        const StartPos& p = starts[i];
        line(3, "[special" + std::to_string(i) + "]");
        line(4, "{");
        line(4, "specialwhat=StartPos" + std::to_string(p.number) + ";");
        line(4, "XPos=" + std::to_string(p.xpos * 16) + ";");
        line(4, "ZPos=" + std::to_string(p.zpos * 16) + ";");
        line(4, "}");
    }
    line(3, "}");   // specials
    line(2, "}");   // Schema 0
    line(1, "}");   // GlobalHeader
    std::string extended;
    auto preserve = [&](const char* key, const std::string& value) {
        if (legacyText(value) != value) extended += std::string("\t") + key + "=" + hexText(value) + ";\r\n";
    };
    preserve("copyright", copyright);
    preserve("missionname", missionName);
    preserve("missiondescription", missionDescription);
    preserve("tageneratorrecipe", generatorRecipe);
    preserve("kingdom", kingdom);
    preserve("useonlyunits", useOnlyUnits);
    preserve("type", mapType);
    preserve("aiprofile", aiProfile);
    if (!extended.empty()) o += "[TAKText]\r\n{\r\n" + extended + "}\r\n";
    return o;
}

} // namespace ta::tnt
