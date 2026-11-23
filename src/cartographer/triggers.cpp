#include "cartographer/triggers.h"

namespace cart {

namespace {

PKind kindOf(const std::string& ph) {
    if (ph == "unit type") return PKind::UnitType;
    if (ph == "player") return PKind::Player;
    if (ph == "location") return PKind::Location;
    if (ph == "text string") return PKind::Text;
    if (ph == "flag") return PKind::Flag;
    return PKind::Value;   // value / value1 / value2
}

// Build an OpDef from a template, deriving its param kinds from the <...>
// placeholders in order.
OpDef make(const char* templ) {
    OpDef d;
    d.templ = templ;
    const std::string& t = d.templ;
    for (size_t i = 0; i < t.size();) {
        if (t[i] == '<') {
            size_t e = t.find('>', i);
            if (e == std::string::npos) break;
            d.params.push_back(kindOf(t.substr(i + 1, e - i - 1)));
            i = e + 1;
        } else ++i;
    }
    return d;
}

// Internal-opcode order (== stored value). Conditions: display == internal.
const char* kCond[] = {
    "Start of game",                                                 // 0
    "Gametime greater than <value>",                                 // 1
    "Gametime less than <value>",                                    // 2
    "Timer <value1> greater than <value2>",                          // 3
    "Timer <value1> less than <value2>",                             // 4
    "I killed more than <value> <unit type>",                        // 5
    "I killed less than <value> <unit type>",                        // 6
    "I killed the most <unit type>",                                 // 7
    "I killed the least <unit type>",                                // 8
    "I lost more than <value> <unit type>",                          // 9
    "I lost less than <value> <unit type>",                          // 10
    "I lost the most <unit type>",                                   // 11
    "I lost the least <unit type>",                                  // 12
    "I control the most <unit type> at <location>",                  // 13
    "I control the least <unit type> at <location>",                 // 14
    "I control more than <value> <unit type> at <location>",         // 15
    "I control less than <value> <unit type> at <location>",         // 16
    "Flag <flag> more than <value>",                                 // 17
    "Flag <flag> less than <value>",                                 // 18
    "Always",                                                        // 19
    "Never",                                                         // 20
    "Opponents left in game is less than <value>",                   // 21
    "Opponents left in game is greater than <value>",                // 22
    "Random: <value> percent of the time it will be true",           // 23
    "Resources less than <value>",                                   // 24
    "Resources greater than <value>",                                // 25
};

// Actions in INTERNAL opcode order (the value the .crt stores).
const char* kAct[] = {
    "Set countdown timer <value1> to <value2>",                      // 0
    "Set countup timer <value1> to <value2>",                        // 1
    "Set flag <flag> to <value>",                                    // 2
    "Add <value> to flag <flag>",                                    // 3
    "Sub <value> to flag <flag>",                                    // 4
    "Victory for me",                                                // 5
    "Defeat for me",                                                 // 6
    "Create <unit type> at <location>",                              // 7
    "Destroy <unit type> at <location>",                             // 8
    "I own all <unit type> at <location>",                           // 9
    "Heal <unit type> by <value> at <location>",                     // 10
    "Damage <unit type> by <value> at <location>",                   // 11
    "Display gameclock",                                             // 12
    "Display <player> <text string>",                                // 13
    "Disable rule",                                                  // 14
    "Move all <unit type> at <location> to <location>",              // 15
    "Set resource limit to <value>",                                 // 16
    "Set resources to <value>",                                      // 17
    "Add <value> to resources",                                      // 18
    "Sub <value> from resources",                                    // 19
    "Set resources normal",                                          // 20
    "Victory for me and all of my teammates",                        // 21
    "Defeat for me and all of my teammates",                         // 22
    "Victory for all of my opponents",                               // 23
    "Defeat for all of my opponents",                                // 24
    "Display <player> <text string> <flag> <text string>",           // 25
};

std::vector<OpDef> buildDefs(const char** src, size_t n) {
    std::vector<OpDef> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(make(src[i]));
    return out;
}

} // namespace

const std::vector<OpDef>& conditionDefs() {
    static const std::vector<OpDef> defs = buildDefs(kCond, sizeof(kCond) / sizeof(*kCond));
    return defs;
}

const std::vector<OpDef>& actionDefs() {
    static const std::vector<OpDef> defs = buildDefs(kAct, sizeof(kAct) / sizeof(*kAct));
    return defs;
}

std::string defaultParam(PKind k) {
    switch (k) {
        case PKind::UnitType: return "Any Unit";
        case PKind::Player:   return "All Players";
        case PKind::Location: return "Anywhere";
        case PKind::Flag:     return "a";
        case PKind::Text:     return "";
        case PKind::Value:    return "0";
    }
    return "";
}

const char* paramLabel(PKind k) {
    switch (k) {
        case PKind::UnitType: return "UNIT TYPE";
        case PKind::Player:   return "PLAYER";
        case PKind::Location: return "LOCATION";
        case PKind::Flag:     return "FLAG";
        case PKind::Text:     return "TEXT";
        case PKind::Value:    return "VALUE";
    }
    return "PARAM";
}

std::string formatRule(bool isAction, const tak::crt::Rule& r) {
    const auto& defs = isAction ? actionDefs() : conditionDefs();
    if (r.opcode < 0 || r.opcode >= int(defs.size()))
        return std::string(isAction ? "act " : "cond ") + std::to_string(r.opcode);
    const std::string& t = defs[size_t(r.opcode)].templ;
    std::string out;
    int slot = 0;
    for (size_t i = 0; i < t.size();) {
        if (t[i] == '<') {
            size_t e = t.find('>', i);
            if (e == std::string::npos) { out += t.substr(i); break; }
            std::string v = slot < 5 ? r.slot[slot] : std::string();
            out += v.empty() ? "_" : v;
            ++slot;
            i = e + 1;
        } else out += t[i++];
    }
    return out;
}

} // namespace cart
