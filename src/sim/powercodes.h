#pragma once

// Cheat codes: retail's group-2 console commands (TotalA.exe command table
// 0x501f48: Radar, ATM, View, LOS, Mapping, DoubleShot, HalfShot, NowISee,
// Meteor, MakePoster), typed in the chat box as +CODE. The chat handler runs a
// '+' line with command mask 1 (group 1, the always-on commands), plus 2 when
// cheats are allowed (global 0x5091cc), or 7 in developer mode
// (0x493e04..0x493e2f); the executor calls a handler only when its group is in
// the mask (0x4b79cd). 0x5091cc is set at game start by game type (0x4971c7):
// a campaign never, a skirmish always, a multiplayer game from the host's
// battle-room "Cheat Codes" box (LOUNGE2.GUI CHEATING; option word bit 13,
// 0x49730f) -- here GameOptions::powerCodes. Names are compared with _stricmp,
// so they are case-insensitive; an argument is read with atoi (0x4b73e0).
//
// ATM, DoubleShot, HalfShot, LOS, Mapping, NowISee and Meteor change hashed
// sim state and travel as Cmd::PowerCode (World::applyPowerCode); Radar, View
// and MakePoster are local display and never leave the client.

#include <cctype>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ta::sim {

enum class PowerCode : uint8_t {
    None = 0,
    Atm = 1,         // 0x4170c0: the viewed player's (+0x2a43) stored energy and metal += 1000
    RetiredManaMe = 2, // was Kingdoms' MANAME (TotalA.exe has no such code): never parsed or applied
    DoubleShot = 3,  // 0x417030: toggle game flag 0x80 (damage x2, 0x499e05)
    HalfShot = 4,    // 0x417060: toggle game flag 0x100 (damage / 2, 0x499e10); independent of DoubleShot
    Los = 5,         // 0x416d50: toggle line of sight (game +0x14281 bit 1); no argument
    Mapping = 6,     // 0x416d80: toggle the shroud (+0x14281 bit 0)
    NowISee = 7,     // 0x417540: line of sight and shroud both off
    Radar = 8,       // 0x417090: toggle game flag 0x200, the minimap shows every unit (client only)
    MakePoster = 9,  // 0x417600: whole-map picture (client only)
    Meteor = 10,     // 0x417760: no argument starts a meteor strike now (0x438070);
                     // "+Meteor n" arms (n != 0) or disarms the storm (0x437d40 / 0x437d50)
    View = 11,       // 0x416b50: see the game as player n (+0x2a43; client only)
};

// The codes the sim applies (Cmd::PowerCode's targetId); the rest are client-side.
constexpr bool simPowerCode(int32_t c) {
    return (c >= int32_t(PowerCode::Atm) && c <= int32_t(PowerCode::NowISee) && c != int32_t(PowerCode::RetiredManaMe)) ||
           c == int32_t(PowerCode::Meteor);
}
constexpr bool simPowerCode(PowerCode c) { return simPowerCode(int32_t(c)); }

// Cmd::PowerCode's queue byte. Only Meteor takes one: its atoi'd argument,
// nonzero (On) or zero (Off); None is the bare code. Invalid is kept so the
// byte's range stays what older builds accepted; nothing sends it.
enum class PowerArg : uint8_t { None = 0, On = 1, Off = 2, Invalid = 3 };

struct PowerCodeInput {
    PowerCode code = PowerCode::None;
    PowerArg arg = PowerArg::None;
    int value = 0;   // the argument, atoi'd (View's player index, Meteor's switch)
};

inline const char* powerCodeName(PowerCode c) {
    switch (c) {
        case PowerCode::Atm: return "ATM";
        case PowerCode::DoubleShot: return "DOUBLESHOT";
        case PowerCode::HalfShot: return "HALFSHOT";
        case PowerCode::Los: return "LOS";
        case PowerCode::Mapping: return "MAPPING";
        case PowerCode::NowISee: return "NOWISEE";
        case PowerCode::Radar: return "RADAR";
        case PowerCode::MakePoster: return "MAKEPOSTER";
        case PowerCode::Meteor: return "METEOR";
        case PowerCode::View: return "VIEW";
        default: return "";
    }
}

namespace detail {
inline bool powerSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline bool powerEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
// The CRT's atoi: an optional sign, then digits; anything else ends it ("on" is 0).
inline int powerAtoi(std::string_view s) {
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
    long long v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && v < 0x7fffffff) v = v * 10 + (s[i++] - '0');
    return int(negative ? -v : v);
}
}  // namespace detail

// A chat line that names one of the codes: "+CODE [argument]", leading and
// trailing whitespace (and whitespace after the '+') ignored, case-insensitive.
// nullopt for anything else -- ordinary chat, another +word, or an empty "+".
// The first word after the code is its argument (only View and Meteor read one).
inline std::optional<PowerCodeInput> parsePowerCode(std::string_view line) {
    using detail::powerSpace;
    size_t i = 0;
    while (i < line.size() && powerSpace(line[i])) ++i;
    if (i >= line.size() || line[i] != '+') return std::nullopt;
    ++i;
    while (i < line.size() && powerSpace(line[i])) ++i;
    size_t end = i;
    while (end < line.size() && !powerSpace(line[end])) ++end;
    const std::string_view name = line.substr(i, end - i);
    while (end < line.size() && powerSpace(line[end])) ++end;
    size_t argEnd = end;
    while (argEnd < line.size() && !powerSpace(line[argEnd])) ++argEnd;
    const std::string_view word = line.substr(end, argEnd - end);
    PowerCodeInput in;
    for (int c = int(PowerCode::Atm); c <= int(PowerCode::View); ++c)
        if (*powerCodeName(PowerCode(c)) && detail::powerEqual(name, powerCodeName(PowerCode(c)))) in.code = PowerCode(c);
    if (in.code == PowerCode::None) return std::nullopt;
    if (!word.empty()) {
        in.value = detail::powerAtoi(word);
        // 0x41776b: Meteor reads its argument when it has one; View always does
        // (a missing one is 0, 0x4b73e0's default).
        if (in.code == PowerCode::Meteor) in.arg = in.value != 0 ? PowerArg::On : PowerArg::Off;
    }
    return in;
}

}  // namespace ta::sim
