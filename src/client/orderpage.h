#pragma once
// The sidebar's ORDERS page (<SIDE>GEN.GUI) for a selection, as TotalA.exe sets
// it up (docs/retail-gui.md section 3, "Order buttons").
//
// 0x41b2e0 walks the local player's selected units (0x41b3a6..0x41b524) and folds
// each one's type order word (UnitType +0x245, sim UnitType::orderFlags) and its
// own states into the game's selection summary (+0x37ebe..+0x37ec2,
// 0x41b54d..0x41b73c):
//   - an order button is enabled when ANY selected unit has its bit;
//   - FIREORD / MOVEORD: the fire / move state of the units that have the bit, 3
//     when they differ, 4 when none has it (0x41b405..0x41b449);
//   - ONOFF: the activated bit of the onoffable units, 2 when they differ, 3 when
//     there are none (0x41b458..0x41b472, 0x41b893..0x41b8b5);
//   - CLOAK: the first cloaker's cloak order, 2 as soon as there is a second one
//     (whatever its order), 3 when there are none (0x41b485..0x41b497).
// 0x41b0f0 loads <SIDE>GEN.GUI afresh (every gadget shown and enabled, as the
// file has it) and 0x41a120 applies the summary: it greys a button out, and
// hides nothing but LOAD or BLAST, which share a slot.

#include "sim/sim.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ta::client {

// One selected unit of the local player.
struct OrderPageUnit {
    uint16_t flags = 0;         // UnitType::orderFlags
    uint8_t moveState = 0;      // unit +0x110 bits 18-19
    uint8_t fireState = 0;      // unit +0x110 bits 20-21
    bool active = false;        // unit +0x10e bit 0
    bool cloakOn = false;       // unit +0x110 bit 11
};

// A gadget's state; the name leaves out the side prefix (ARM / COR).
struct OrderButton {
    std::string_view name;
    bool shown = true;
    bool grayed = false;
    int stage = -1;             // the status 0x41a120 writes (0x4a11c0); -1: none
};

// The selection summary of 0x41b2e0.
struct OrderSummary {
    uint16_t any = 0;           // OR of the order words
    int fire = 4, move = 4;     // 4: none has the bit; 3: mixed
    int onOff = 3, cloak = 3;   // 3: none; 2: mixed
};

inline OrderSummary orderSummary(const std::vector<OrderPageUnit>& units) {
    using T = ta::sim::UnitType;
    OrderSummary s;
    bool firstCloak = true;
    for (const auto& u : units) {
        s.any |= u.flags;
        if (u.flags & T::kOrderFireState)                                    // 0x41b3fc..0x41b420
            s.fire = s.fire == 4 ? (u.fireState & 3) : (s.fire == (u.fireState & 3) ? s.fire : 3);
        if (u.flags & T::kOrderMoveState)                                    // 0x41b425..0x41b449
            s.move = s.move == 4 ? (u.moveState & 3) : (s.move == (u.moveState & 3) ? s.move : 3);
        if (u.flags & T::kOrderOnOff)                                        // 0x41b44e.., 0x41b893..
            s.onOff = s.onOff == 3 ? int(u.active) : (s.onOff == int(u.active) ? s.onOff : 2);
        if (u.flags & T::kOrderCloak) {                                      // 0x41b47b..0x41b497
            s.cloak = firstCloak ? int(u.cloakOn) : 2;
            firstCloak = false;
        }
    }
    return s;
}

// The page 0x41a120 sets up. `tabs`: exactly one unit is selected and its type has
// build pages (type +0x22e, 0x41a14b / 0x41a18e); otherwise BUILD and ORDERS are
// greyed out.
inline std::vector<OrderButton> orderPage(const OrderSummary& s, bool tabs) {
    using T = ta::sim::UnitType;
    std::vector<OrderButton> b;
    auto add = [&](std::string_view n, bool shown, bool grayed, int stage = -1) {
        b.push_back(OrderButton{n, shown, grayed, stage});
    };
    add("BUILD", true, !tabs);                                               // 0x41a134..0x41a179
    add("ORDERS", true, !tabs);                                              // 0x41a17e..0x41a1be
    // The state buttons: grey when none has the order, else the state's frame.
    add("CLOAK", true, s.cloak == 3, s.cloak == 3 ? -1 : s.cloak);           // 0x41a1c3..0x41a1fe
    add("ONOFF", true, s.onOff == 3, s.onOff == 3 ? -1 : s.onOff);           // 0x41a203..0x41a23e
    add("MOVEORD", true, s.move == 4, s.move == 4 ? -1 : s.move);            // 0x41a243..0x41a27b
    add("FIREORD", true, s.fire == 4, s.fire == 4 ? -1 : s.fire);            // 0x41a280..0x41a2c1
    const std::pair<std::string_view, uint16_t> orders[] = {                 // 0x41a2c6..0x41a403
        {"MOVE", T::kOrderMove},     {"STOP", T::kOrderStop},       {"ATTACK", T::kOrderAttack},
        {"DEFEND", T::kOrderGuard},  {"PATROL", T::kOrderPatrol},   {"RECLAIM", T::kOrderReclaim},
        {"REPAIR", T::kOrderRepair}, {"CAPTURE", T::kOrderCapture}};
    for (const auto& [n, bit] : orders) add(n, true, !(s.any & bit));
    if (s.any & T::kOrderLoad) {          // a transport: LOAD and UNLOAD, BLAST hidden (0x41a471..0x41a485)
        add("LOAD", true, false);
        add("UNLOAD", true, false);
        add("BLAST", false, false);
    } else {                              // LOAD hidden, UNLOAD grey (0x41a412..0x41a43f); BLAST is the D-gun
        add("LOAD", false, false);
        add("UNLOAD", true, true);
        add("BLAST", true, !(s.any & T::kOrderDgun));                        // 0x41a444..0x41a466
    }
    return b;
}

// A press on a state button sets the whole selection to the next state
// (handler 0x41a490): MOVEORD / FIREORD go 0 -> 1 -> 2 -> 0 and mixed -> 0
// (jump tables 0x41a900 / 0x41a910); ONOFF off -> ACTIVATE, on -> DEACTIVATE,
// mixed -> ACTIVATE (0x41a7db..0x41a88f); CLOAK on or mixed -> CLOAK_OFF, off
// -> CLOAK_ON (0x41a743..0x41a7a5).
inline int nextStandingState(int state) { return state == 0 ? 1 : state == 1 ? 2 : 0; }
inline bool nextOnOff(int state) { return state != 1; }
inline bool nextCloak(int state) { return state == 0; }

}  // namespace ta::client
