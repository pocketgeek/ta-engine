#pragma once

namespace ta {

// The display VM's Activate / Deactivate mirror (GameView's per-tick pass).
// The simulation owns activation; the display VM replays its edges.
//  - on/off units and activatewhenbuilt units follow the unit's flag
//    (manual gate commands, the AI's gate checks, 512301 / 429a8d);
//  - a flyer has neither key, yet every TA aircraft script opens its wings in
//    Activate and folds them in Deactivate (armfig: RequestState -> activatescr
//    / deactivatescr). TotalA.exe sets the activation bit at each take-off
//    (0x43d210 mode 2 -> 0x48b090(1, 1), and the missions' stage 0, e.g.
//    0x40fb4d) and clears it as the landing starts down (VTOL_LandIfCan
//    0x40f4c7) and on touchdown (0x43d210 mode 1 -> 0x48b090(1, 0)); the
//    flyer follows the script's bit (the sim's GET ACTIVATION word).
// Returns 1 to start Activate, -1 to start Deactivate, 0 for no call-in, and
// updates `shown` (the state the display VM was last given).
inline int retailDisplayActivation(bool& shown,bool hasActivate,bool flyer,bool onOffable,
                                   bool activateWhenBuilt,bool scriptActivated,bool unitActive) {
    if (!hasActivate || !(flyer || onOffable || activateWhenBuilt)) return 0;
    const bool now=flyer ? scriptActivated : unitActive;
    if (now==shown) return 0;
    shown=now;
    return now ? 1 : -1;
}

} // namespace ta
