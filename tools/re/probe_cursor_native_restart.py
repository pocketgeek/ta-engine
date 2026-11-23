#!/usr/bin/env python3
"""Exercise retail's live cursor sequence switch and frame updater headlessly.

The fixture supplies two synthetic registered cursor sequences and a tiny frame
table, then calls the real 0x575e30 virtual setter and 0x58a080 native updater
from KINGDOMS.icd. It tests clock/state behavior, not cursor art or input.
"""
import struct

from emu import HEAP, Icd


SET_DEFAULT = 0x575E30
UPDATE_FRAME = 0x58A080
MANAGER_VTABLE = 0x5F571C


def put_u32(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def get_u32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def set_sequence(uc, manager, sequence_id, frame_ids):
    # Each 0x20-byte sequence record starts 4 bytes into the manager; native
    # 0x58a080 reads the frame index at slot+0x10 and [begin,end) at +0x18/+1c.
    record = manager + sequence_id * 0x20
    frame_list = HEAP + 0x38000 + sequence_id * 0x100
    put_u32(uc, record + 0x10, 0)
    put_u32(uc, record + 0x18, frame_list)
    put_u32(uc, record + 0x1C, frame_list + len(frame_ids) * 4)
    uc.mem_write(frame_list, struct.pack("<%dI" % len(frame_ids), *frame_ids))


def call(icd, address, *, this, args=()):
    result, error = icd.call(address, args=args, ecx=this)
    assert error is None, error
    return result


def main():
    icd = Icd()
    uc = icd.uc
    manager = HEAP + 0x20000
    frame_table = HEAP + 0x30000
    uc.mem_write(manager, bytes(0x420))
    put_u32(uc, manager, MANAGER_VTABLE)
    put_u32(uc, manager + 0x404, frame_table)
    put_u32(uc, manager + 0x409, 0xFFFFFFFF)  # default sequence
    put_u32(uc, manager + 0x40D, 0xFFFFFFFF)  # no active override
    put_u32(uc, manager + 0x411, 0)           # native shared countdown
    put_u32(uc, 0x65DDCC, manager)             # service used by 0x575e30

    set_sequence(uc, manager, 0, [100, 101, 102])
    set_sequence(uc, manager, 1, [200, 201, 202])
    queried_frames = []

    def frame_delay_lookup(uc, sp):
        (frame_id,) = struct.unpack("<I", uc.mem_read(sp, 4))
        queried_frames.append(frame_id)
        return 1, 2

    # Keep the updater's sequence/index/countdown branches native while
    # supplying a uniform synthetic two-tick authored-frame delay.
    icd.hooks[0x58D9F0] = frame_delay_lookup
    icd.freeze_hooks()

    def select(sequence_id):
        call(icd, SET_DEFAULT, this=manager, args=(sequence_id,))
        assert get_u32(uc, manager + 0x409) == sequence_id

    def update():
        call(icd, UPDATE_FRAME, this=manager)

    def state(sequence_id):
        record = manager + sequence_id * 0x20
        return get_u32(uc, record + 0x10), get_u32(uc, manager + 0x411)

    select(0)
    update()
    assert state(0) == (1, 2), state(0)
    print(f"sequence A after first native update: frame {state(0)[0]}, countdown {state(0)[1]}")

    # Switching only writes +0x409. Its own animation index is retained; the
    # shared timer is consumed while the second sequence is selected.
    select(1)
    assert state(0) == (1, 2), state(0)
    update()
    assert state(1) == (0, 1), state(1)
    update()
    assert state(1) == (0, 0), state(1)
    update()
    assert state(1) == (1, 2), state(1)
    print(f"sequence B after shared countdown expires: frame {state(1)[0]}, countdown {state(1)[1]}")

    select(0)
    assert state(0) == (1, 2), state(0)
    update()
    assert state(0) == (1, 1), state(0)
    update()
    assert state(0) == (1, 0), state(0)
    update()
    assert state(0) == (2, 2), state(0)
    print(f"sequence A after returning: frame {state(0)[0]}, countdown {state(0)[1]}")
    assert queried_frames == [101, 201, 102], queried_frames
    print("PASS: native default-sequence switches retain per-sequence frame indices and share one countdown")
    print("Frame-delay metadata is stubbed uniformly at two ticks; synthetic IDs. No GAF renderer,")
    print("pointer input, or retail GUI exercised.")


if __name__ == "__main__":
    main()
