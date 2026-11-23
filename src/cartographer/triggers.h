#pragma once

// Cartographer's trigger opcode tables + rule formatting. The scenario rules
// live in the map's .crt as per-player groups of condition/action records
// (opcode + five string operand slots), read/written by the shared tak::crt.
// This provides the human-readable templates for the 26 conditions and 26
// actions (reverse-engineered from Cartographer.exe's tables at 0x51c190) so
// the editor can render a rule and know each opcode's parameter shape.
//
// A template's `<...>` placeholders are the operand slots in order: e.g.
// condition 15 "I control more than <value> <unit type> at <location>" maps
// slot0=value, slot1=unit type, slot2=location. Numeric params are stored as
// ASCII in the slot. Verified against shipped .crt (Ulin's Folly op15 =
// ['2','ARAAT','hill']).

#include "crt/crt.h"

#include <string>
#include <vector>

namespace cart {

enum class PKind { Value, UnitType, Player, Location, Text, Flag };

struct OpDef {
    std::string templ;            // full template, with <...> placeholders
    std::vector<PKind> params;    // one per placeholder, in slot order
};

// The 26 condition / 26 action opcode definitions, indexed by internal opcode
// (the value stored in the .crt). Actions use the internal order (the retail
// dialog shows a remapped display order, but the stored opcode is the internal).
const std::vector<OpDef>& conditionDefs();
const std::vector<OpDef>& actionDefs();

// Human-readable rendering of a rule: the opcode's template with each <...>
// filled from the rule's slots. Unknown opcodes render as "cond/act <n>".
std::string formatRule(bool isAction, const tak::crt::Rule& r);

// The default operand for a parameter kind (retail combo defaults).
std::string defaultParam(PKind k);

// A short label for a parameter kind (for the param editor).
const char* paramLabel(PKind k);

} // namespace cart
