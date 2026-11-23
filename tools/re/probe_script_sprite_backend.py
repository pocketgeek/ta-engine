#!/usr/bin/env python3
"""Establish the backend reached by the script particle sprite entry point.

Runs 536e90, its real clipping routine and surface accessors. Only the two
CPU blitters are sinks; this does not emulate their pixel arithmetic.
"""
import struct
from emu import Icd, HEAP
p = Icd()
frame, surface, context, pixels = [HEAP + i * 0x10000 for i in range(4)]
calls = []
def put(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
def raw(uc, sp):
    args = struct.unpack('<6I', uc.mem_read(sp, 24))
    calls.append(('raw', args[0], args[4], args[5]))
    return 0, 0 # cdecl: caller removes arguments

def compressed(uc, sp):
    args = struct.unpack('<6I', uc.mem_read(sp, 24))
    calls.append(('compressed', args[0], args[1], args[3], args[5]))
    return 0, 0
p.hooks.update({0x549f4c: raw, 0x54a077: compressed})
p.freeze_hooks()
put(0x641a48, context); put(context + 0x24, 1234)
put(surface + 8, 640); put(surface + 12, pixels)
p.uc.mem_write(surface + 0x1c, struct.pack('<4i', 0, 0, 639, 479))
p.uc.mem_write(frame, struct.pack('<4h', 16, 12, 3, 4))
p.uc.mem_write(frame + 8, bytes((42, 0, 0, 0)))
put(frame + 0x10, pixels + 0x1000)
for encoding in range(256):
    p.uc.mem_write(frame + 9, bytes((encoding,)))
    for x, y, visible in ((100, 200, True), (-100, -100, False), (640, 480, True)):
        calls.clear()
        _, error = p.call(0x536e90, (surface, frame, x, y))
        assert not error, (encoding, error)
        expected = []
        if visible:
            expected = [('raw', surface, 42, 1234)] if encoding == 0 else [
                ('compressed', pixels, 640, pixels + 0x1000, 1234)]
        assert calls == expected, (encoding, x, y, calls, expected)
print('PASS: 768 script-sprite cases reach CPU raw/compressed blitters after clipping, without a Glide primitive submission')
