"""Bounded fixed GlobalAlloc/GlobalLock host for simulation-only replays.

This uses the replay's existing allocator. Movable handles and handles from
outside this host remain explicit errors; no game routine is substituted.
"""


class CapturedFixedGlobalMemory:
    def __init__(self, process):
        self.process = process
        self.allocations = {}
        for slot, callback in ((0x5eb0a8, self.allocate), (0x5eb1f0, self.lock)):
            function = process.u32(slot)
            process.ensure(function, 1)
            process.icd.hooks[function] = callback

    def allocate(self, uc, args):
        p = self.process
        flags, size = p.u32(args), p.u32(args+4)
        if flags & ~0x40:
            raise RuntimeError(f'unsupported GlobalAlloc flags: {flags:#x}')
        _, pointer = p.allocate(uc, args+4)
        self.allocations[pointer] = size
        if flags & 0x40:
            p.put(pointer, bytes(size))
        return 2, pointer

    def lock(self, uc, args):
        pointer = self.process.u32(args)
        if pointer and pointer not in self.allocations:
            raise RuntimeError(f'uncaptured GlobalLock handle: {pointer:#x}')
        return 1, pointer
