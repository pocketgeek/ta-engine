#!/usr/bin/env python3
"""Read-only JSONL roster poller for a live retail Kingdoms game (AI comparison).

  python3 tools/re/retail_ai_poll.py PID --every 30 --max-ticks 18000 \
      --output ~/tak-tmp/retail-ai/runs/x/retail.jsonl [--squads-every 150]

It reads game memory through /proc/PID/mem with pread and never writes it.
Field offsets are the ones tools/re/livesample.py, decode_ai_state.py and
decode_player_cache.py already use; verify_code() checks the mapped binary
against assets/game/KINGDOMS.icd (v4.0BB) first and refuses anything else.

Output (one JSON object per line; the same schema tak's TAK_ROSTER_DUMP writes,
so tools/re/compare_ai.py reads both):
  {"kind":"meta","schema":1,"engine":"retail",...}
  {"kind":"sample","tick":T,"players":[{"p","controller","mana","storage"}],
   "units":[[id,player,type,remaining,x,z],...],"squads":[...]?}
  {"kind":"end","tick":T,"reason":...}
remaining is retail's construction-remaining fraction (1 = just placed,
0 = finished). x and z are world pixels. Ticks are retail's 30 Hz ticks.

Sampling: the poller spins on the tick counter and takes the first tick at or
after each multiple of --every; a read is kept only if the tick did not change
and the game RNG did not change while reading it (otherwise it retries). With -AutoStop retail runs about
140-280 ticks/s, so a sample can land a few ticks after its target; each
record carries the tick actually read.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from livesample import Mem, verify_code  # noqa: E402

GAME_PTR = 0x62d55c
RNG_STATE = 0x64186c   # the game RNG (Park-Miller state), for determinism checks
TICK_OFF = 0x19f44
ENTITY_TABLE = 0x14e84
STRIDE = 0x138
PLAYER_BASE = 0x2404
PLAYER_STRIDE = 0x110
MAX_PLAYERS = 10
CONTROLLER = {1: 'human', 2: 'ai', 3: 'remote'}
# Squad think handlers (decode_ai_state.py); names follow PLAN.md section 1.4.
SQUAD_KIND = {0x40b320: 'base', 0x40e180: 'strike', 0x40ea30: 'backup',
              0x40eb40: 'vtol', 0x4101e0: 'homeless'}
SCHEMA = 1


class RetailReader:
    def __init__(self, pid, verify=True):
        self.mem = Mem(pid)
        if verify:
            verify_code(self.mem)
        self.names = {}

    def close(self):
        self.mem.close()

    def u32(self, address):
        return self.mem.u32(address)

    def game(self):
        return self.u32(GAME_PTR)

    def tick(self):
        game = self.game()
        return self.u32(game + TICK_OFF) if game else 0

    def type_name(self, address):
        if address not in self.names:
            raw = self.mem.read(address + 32, 32).split(b'\0', 1)[0]
            self.names[address] = raw.decode('latin1').lower()
        return self.names[address]

    def players(self, game):
        out = []
        for p in range(MAX_PLAYERS):
            record = game + PLAYER_BASE + p * PLAYER_STRIDE
            if not self.u32(record):
                continue
            kind = self.mem.read(record + 0xea, 1)[0]
            resources = self.u32(record + 0x10c)
            mana, storage = struct.unpack('<2f', self.mem.read(resources, 8)) if resources else (0.0, 0.0)
            out.append({'p': p, 'controller': CONTROLLER.get(kind, str(kind)),
                        'mana': round(mana, 1), 'storage': round(storage, 1)})
        return out

    def units(self, game):
        base, end = struct.unpack('<II', self.mem.read(game + ENTITY_TABLE, 8))
        count = (end - base) // STRIDE
        if not base or not 0 < count <= 20000:
            return []
        records = self.mem.read(base, count * STRIDE)
        units = []
        for slot in range(count):
            off = slot * STRIDE
            if not struct.unpack_from('<I', records, off + 0x130)[0] & 0x1000000:
                continue
            owner = struct.unpack_from('<I', records, off + 0xb8)[0]
            player = (owner - game - PLAYER_BASE) // PLAYER_STRIDE if owner else -1
            type_address = struct.unpack_from('<I', records, off + 0xb4)[0]
            remaining = struct.unpack_from('<f', records, off + 0x108)[0]
            x, _, z = struct.unpack_from('<iii', records, off + 0x68)
            units.append([slot, player, self.type_name(type_address) if type_address else '?',
                          round(remaining, 3), x >> 16, z >> 16])
        return units

    def squads(self, game, entity_base):
        out = []
        for p in range(MAX_PLAYERS):
            record = game + PLAYER_BASE + p * PLAYER_STRIDE
            if not self.u32(record):
                continue
            ai = self.u32(record + 0x80)
            if not ai or self.mem.read(record + 0xea, 1)[0] != 2:
                continue
            squads = []
            for slot in range(100):
                pointer = self.u32(ai + 0x11 + slot * 4)
                if not pointer:
                    continue
                data = self.u32(pointer + 8)
                handler = self.u32(self.u32(pointer))
                begin, end, _ = struct.unpack('<3I', self.mem.read(data + 0xb8, 12))
                members = []
                if end > begin and (end - begin) % 4 == 0 and end - begin <= 80000:
                    members = [(a - entity_base) // STRIDE for a in
                               struct.unpack(f'<{(end - begin) // 4}I', self.mem.read(begin, end - begin))]
                if not members:
                    continue
                squads.append({'slot': slot, 'kind': SQUAD_KIND.get(handler, hex(handler)),
                               'deadline': self.u32(pointer + 12),
                               'params': list(struct.unpack('<9i', self.mem.read(data + 12, 36))),
                               'members': members})
            out.append({'p': p, 'squads': squads})
        return out

    def sample(self, with_squads=False):
        """One consistent sample, or None if the tick moved while reading."""
        game = self.game()
        if not game:
            return None
        tick = self.u32(game + TICK_OFF)
        record = {'kind': 'sample', 'tick': tick, 'rng': self.u32(RNG_STATE),
                  'players': self.players(game), 'units': self.units(game)}
        if with_squads:
            entity_base = self.u32(game + ENTITY_TABLE)
            record['squads'] = self.squads(game, entity_base)
        # The tick counter moves at the START of a tick step, so an unchanged tick
        # does not prove the step was idle; an RNG that moved during the read
        # catches most reads that overlapped the AI or sim of that tick.
        if self.u32(game + TICK_OFF) != tick or self.u32(RNG_STATE) != record['rng']:
            return None
        return record


def poll(pid, output, every=30, max_ticks=18000, squads_every=0, meta=None,
         stall_seconds=20.0, stop_on_defeat=True, log=None, resume=False, write_end=True):
    """Poll until max_ticks, a stall, or one side left; returns the end record.
    resume=True appends to an existing capture (after a pause such as PrintWeights)."""
    reader = RetailReader(pid)
    target = 0
    last_tick, last_change = -1, time.monotonic()
    reason = 'max-ticks'
    end_tick = 0
    with open(output, 'a' if resume else 'x') as out:
        if not resume:
            out.write(json.dumps({'kind': 'meta', 'schema': SCHEMA, 'engine': 'retail',
                                  'tick_rate': 30, 'every': every, **(meta or {})}) + '\n')
        try:
            while True:
                try:
                    tick = reader.tick()
                except (OSError, RuntimeError):
                    reason = 'process-gone'
                    break
                now = time.monotonic()
                if tick != last_tick:
                    last_tick, last_change = tick, now
                elif now - last_change > stall_seconds:
                    reason = 'stalled'
                    break
                if tick >= target and tick > 0:
                    want_squads = bool(squads_every) and (target % squads_every == 0)
                    try:
                        record = reader.sample(want_squads)
                    except (OSError, RuntimeError, struct.error):
                        record = None
                    if record is None:
                        continue
                    record['target'] = target
                    out.write(json.dumps(record, separators=(',', ':')) + '\n')
                    out.flush()
                    end_tick = record['tick']
                    target = (record['tick'] // every + 1) * every
                    if record['tick'] >= max_ticks:
                        break
                    if stop_on_defeat and record['tick'] > 300:
                        owners = {u[1] for u in record['units']}
                        alive = [p['p'] for p in record['players'] if p['p'] in owners]
                        if len(alive) <= 1:
                            reason = 'one-player-left'
                            break
                    if log and record['tick'] // every % 200 == 0:
                        log(f'tick {record["tick"]}: {len(record["units"])} units')
                    continue
                time.sleep(0.0005)
        finally:
            if write_end or reason != 'max-ticks':
                out.write(json.dumps({'kind': 'end', 'tick': end_tick, 'reason': reason}) + '\n')
            reader.close()
    return {'tick': end_tick, 'reason': reason}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('pid', type=int)
    parser.add_argument('--output', required=True, help='new JSONL file')
    parser.add_argument('--every', type=int, default=30, help='sample cadence in ticks (default 30 = 1 s)')
    parser.add_argument('--max-ticks', type=int, default=18000, help='stop at this tick (default 10 min)')
    parser.add_argument('--squads-every', type=int, default=150,
                        help='also decode AI squads on every Nth-tick target (0 = never)')
    args = parser.parse_args()
    if args.every < 1 or (args.squads_every and args.squads_every % args.every):
        parser.error('--every must be >= 1 and divide --squads-every')
    print(json.dumps(poll(args.pid, args.output, args.every, args.max_ticks, args.squads_every)))


if __name__ == '__main__':
    main()
