#!/usr/bin/env python3
"""What follows a retired VTOL_UNLOAD: the dispatcher's default mission.

KINGDOMS executes the real 0x4d8450 dispatcher and 0x41ae20 VTOL_UNLOAD over the
NativeAirUnload fixture (probe_transport_air_unload_callbacks.py), with the
carrier's FBI default mission (+0x24b) set. Only 0x4d83b0, the default-mission
constructor, is a sink: it records its arguments and whether the step-out
flight controller (navigator +4) is still attached when it is called.

1. Mission retirement. VTOL_UNLOAD's step-out controller aims 2*(range+400) px
   ahead (retailUnloadStepOut). The dispatcher retires the mission after its
   one-tick empty tail and, finding the list empty, constructs the default
   mission IN THE SAME CALL -- with the step-out controller still attached. The
   controller is not a mission and does not defer it.
2. The gate at 0x4d856a: an empty list gets the default mission for owner type
   (+0xea) 1 or 2 with a nonzero type default, whatever the mover is doing.

VTOL_Standby (check_vtol_standby.py) then installs VTOL_LANDIFCAN two ticks
later, whose stage 1 replaces the controller with a 32 px brake point
(check_landing_mission.py). So a retail carrier lands where it dropped its
cargo; it does not fly on to the step-out point.
"""
import struct

import probe_transport_air_unload_callbacks as callbacks

DEFAULT_MISSION = 0x2A   # any nonzero FBI defaultmissiontype index


def native(default_mission, owner_type, hook_log):
    original = callbacks.Icd.freeze_hooks
    holder = {}

    def default_hook(_uc, sp):
        fixture = holder['native']
        unit, mission = struct.unpack('<2I', fixture.p.uc.mem_read(sp, 8))
        controller = fixture.get(fixture.nav + 4)
        hook_log.append((fixture.get(fixture.game + 0x19F44), unit, mission & 0xFF,
                         controller != 0, fixture.get(fixture.carrier + 0x60)))
        return 2, 0

    def freeze(icd):
        icd.hooks[0x4D83B0] = default_hook
        original(icd)

    callbacks.Icd.freeze_hooks = freeze
    try:
        fixture = callbacks.NativeAirUnload()
    finally:
        callbacks.Icd.freeze_hooks = original
    holder['native'] = fixture
    fixture.byte(fixture.kind + 0x24B, default_mission)
    fixture.byte(fixture.owner + 0xEA, owner_type)
    return fixture


def retirement():
    log = []
    fixture = native(DEFAULT_MISSION, 1, log)
    retired = None
    for tick in range(1, 40):
        fixture.dispatch(tick)
        if fixture.get(fixture.carrier + 0x60) == 0:
            retired = tick
            break
    assert retired, 'VTOL_UNLOAD never retired'
    assert fixture.get(fixture.passenger + 0xA8) == 0, 'passenger still aboard'
    assert len(log) == 1, log
    tick, unit, mission, controller, head = log[0]
    assert (tick, unit, mission) == (retired, fixture.carrier, DEFAULT_MISSION), log[0]
    assert controller, 'step-out controller already detached when the default mission was built'
    assert head == 0, 'default mission built with VTOL_UNLOAD still queued'
    print(f'PASS: VTOL_UNLOAD retires at tick {retired} and the same dispatch builds the '
          f'default mission while the step-out controller is still attached')


def gate():
    cases = 0
    for owner_type in range(5):
        for default_mission in (0, DEFAULT_MISSION):
            log = []
            fixture = native(default_mission, owner_type, log)
            fixture.put(fixture.carrier + 0x60, 0)          # empty mission list
            fixture.put(fixture.nav + 4, fixture.controller)  # a controller still flying it
            fixture.dispatch(1)
            expected = default_mission != 0 and owner_type in (1, 2)
            assert bool(log) == expected, (owner_type, default_mission, log)
            if log:
                assert log[0][3], 'controller must still be attached'
            cases += 1
    print(f'PASS: {cases} empty-list cases: the default mission is built for owner '
          f'types 1/2 with a nonzero type default, independent of the attached controller')


if __name__ == '__main__':
    retirement()
    gate()
