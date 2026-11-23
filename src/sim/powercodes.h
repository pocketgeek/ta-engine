#pragma once

// Power Codes: retail's cheat console commands (KINGDOMS.icd command table
// 0x605bc8, group 2), typed in the chat box as +CODE and honoured only when
// the match was created with POWER CODES on (GameOptions::powerCodes; retail's
// "Power Codes" battle-room box, GameOptions+0x18, never in a campaign --
// gate 0x499610). Retail looks names up with _stricmp, so they are
// case-insensitive, and the LOS argument is compared the same way.
//
// Only the nine codes below exist here. Seven change hashed sim state and
// travel as Cmd::PowerCode (World::applyPowerCode); Radar and MakePoster are
// local display only and never leave the client.

#include <cctype>
#include <cstdint>
#include <optional>
#include <string_view>

namespace tak::sim {

enum class PowerCode : uint8_t {
    None = 0,
    Atm = 1,         // 425d30: fill the issuer's mana pool to its storage
    ManaMe = 2,      // 426390: fill each selected unit's personal mana
    DoubleShot = 3,  // 425cd0: toggle doubled damage (clears HalfShot)
    HalfShot = 4,    // 425d00: toggle halved damage (clears DoubleShot)
    Los = 5,         // 4258e0: toggle line of sight; "On"/"Off" set it
    Mapping = 6,     // 425980: toggle the shroud (re-shrouding forgets exploration)
    NowISee = 7,     // 4261a0: line of sight and shroud both off
    Radar = 8,       // 425d70: the minimap shows every unit (client only)
    MakePoster = 9,  // 4261f0: whole-map picture (client only)
};

// The codes the sim applies (Cmd::PowerCode's targetId); the rest are client-side.
constexpr bool simPowerCode(PowerCode c) { return c >= PowerCode::Atm && c <= PowerCode::NowISee; }
constexpr bool simPowerCode(int32_t c) { return c >= int32_t(PowerCode::Atm) && c <= int32_t(PowerCode::NowISee); }

// Cmd::PowerCode's queue byte. Only LOS takes one; retail ignores any other word
// after LOS, so a parse that found one reports Invalid and nothing is applied.
enum class PowerArg : uint8_t { None = 0, On = 1, Off = 2, Invalid = 3 };

// World::damageMode(): 52a330 multiplies the spread damage before truncating it.
enum class DamageMode : uint8_t { Normal = 0, Double = 1, Half = 2 };

struct PowerCodeInput {
    PowerCode code = PowerCode::None;
    PowerArg arg = PowerArg::None;
};

inline const char* powerCodeName(PowerCode c) {
    switch (c) {
        case PowerCode::Atm: return "ATM";
        case PowerCode::ManaMe: return "MANAME";
        case PowerCode::DoubleShot: return "DOUBLESHOT";
        case PowerCode::HalfShot: return "HALFSHOT";
        case PowerCode::Los: return "LOS";
        case PowerCode::Mapping: return "MAPPING";
        case PowerCode::NowISee: return "NOWISEE";
        case PowerCode::Radar: return "RADAR";
        case PowerCode::MakePoster: return "MAKEPOSTER";
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
}  // namespace detail

// A chat line that names one of the nine codes: "+CODE [argument]", leading and
// trailing whitespace (and whitespace after the '+') ignored, case-insensitive.
// nullopt for anything else -- ordinary chat, another +word, or an empty "+".
// Words after the code are ignored except LOS's On/Off (retail's argument test).
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
    for (int c = int(PowerCode::Atm); c <= int(PowerCode::MakePoster); ++c)
        if (detail::powerEqual(name, powerCodeName(PowerCode(c)))) in.code = PowerCode(c);
    if (in.code == PowerCode::None) return std::nullopt;
    if (in.code == PowerCode::Los && !word.empty())
        in.arg = detail::powerEqual(word, "on") ? PowerArg::On
               : detail::powerEqual(word, "off") ? PowerArg::Off : PowerArg::Invalid;
    return in;
}

}  // namespace tak::sim
