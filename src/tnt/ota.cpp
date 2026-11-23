#include "tnt/ota.h"

#include "tdf/tdf.h"

#include <cstdio>

namespace tak::tnt {

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
void restoreText(const tak::tdf::Node* node, const char* key, std::string& text) {
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
    tak::tdf::Node root = tak::tdf::parseText(text, "<ota>");
    const tak::tdf::Node* gh = root.child("globalheader");
    if (!gh) return s;
    s.copyright = gh->valueOr("copyright", s.copyright);
    s.missionName = gh->valueOr("missionname", "");
    s.missionDescription = gh->valueOr("missiondescription", "");
    s.generatorRecipe = gh->valueOr("takgeneratorrecipe", "");
    s.kingdom = gh->valueOr("kingdom", "");
    s.useOnlyUnits = gh->valueOr("useonlyunits", "");
    s.hasScenario = gh->numberOr("hasscenario", 0) != 0;
    // size = "W x H" (Units)
    if (const std::string* sz = gh->value("size"))
        std::sscanf(sz->c_str(), "%d x %d", &s.sizeW, &s.sizeH);
    const tak::tdf::Node* md = gh->child("map data");
    if (md) {
        s.mapType = md->valueOr("type", s.mapType);
        s.aiProfile = md->valueOr("aiprofile", s.aiProfile);
        if (const tak::tdf::Node* sp = md->child("specials")) {
            // [special0], [special1], ... each with specialwhat/XPos/ZPos.
            for (const std::string& nm : sp->childOrder) {
                const tak::tdf::Node* one = sp->child(nm);
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
    restoreText(textFields, "takgeneratorrecipe", s.generatorRecipe);
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
    // A [Section] header sits at depth D; its brace + body sit at D+1.
    line(0, "[GlobalHeader]");
    line(1, "{");
    line(1, "Copyright=" + legacyText(copyright) + ";");
    line(1, "missionname=" + legacyText(missionName) + ";");
    line(1, "missiondescription=" + legacyText(missionDescription) + ";");
    if(!generatorRecipe.empty())line(1, "takgeneratorrecipe=" + legacyText(generatorRecipe) + ";");
    line(1, "kingdom=" + legacyText(kingdom) + ";");
    line(1, "numplayers=" + std::to_string(starts.size()) + ";");
    line(1, "size=" + std::to_string(sizeW) + " x " + std::to_string(sizeH) + ";");
    line(1, "memory=32 MB;");
    if (!useOnlyUnits.empty()) line(1, "useonlyunits=" + legacyText(useOnlyUnits) + ";");
    line(1, std::string("hasscenario=") + (hasScenario ? "1" : "0") + ";");
    line(1, "[Map Data]");
    line(2, "{");
    line(2, "Type=" + legacyText(mapType) + ";");
    line(2, "aiprofile=" + legacyText(aiProfile) + ";");
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
    std::string extended;
    auto preserve = [&](const char* key, const std::string& value) {
        if (legacyText(value) != value) extended += std::string("\t") + key + "=" + hexText(value) + ";\r\n";
    };
    preserve("copyright", copyright);
    preserve("missionname", missionName);
    preserve("missiondescription", missionDescription);
    preserve("takgeneratorrecipe", generatorRecipe);
    preserve("kingdom", kingdom);
    preserve("useonlyunits", useOnlyUnits);
    preserve("type", mapType);
    preserve("aiprofile", aiProfile);
    if (!extended.empty()) o += "[TAKText]\r\n{\r\n" + extended + "}\r\n";
    return o;
}

} // namespace tak::tnt
