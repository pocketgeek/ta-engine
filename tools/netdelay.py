#!/usr/bin/env python3
"""netdelay.py -- a TCP relay that adds latency and jitter, for testing lockstep
multiplayer over something other than a perfect LAN.

    tools/netdelay.py --listen 7700 --to ta.pgnet.us:7900 --rtt 100 --jitter 20

WHY THIS EXISTS. Every multiplayer run so far has been sub-millisecond LAN, which
leaves the latency-sensitive machinery untested: the client's adaptive jitter buffer
sizes itself from measured RTT (netDelay_ = clamp(2 + max(jitter, rtt/60), 2, 16)), the
server may not run more than kMaxLeadTicks=120 ticks past its slowest consumer, and a
spectator heartbeats every 0.4s to keep that flow control fed. None of those do
anything interesting at 0.2ms.

WHY A PROXY RATHER THAN tc netem. netem is the faithful tool -- it delays real packets
-- but it needs root on one of the two machines, and neither has passwordless sudo
here. A userspace relay needs no privileges and is precisely controllable. The
difference matters for what this can claim: netem would also model per-packet
reordering and loss at the IP layer, while this delays a TCP byte stream. For testing
how the ENGINE behaves when bundles arrive late, that is the right abstraction; it is
not a substitute for testing TCP's own behaviour under loss.

ORDERING IS PRESERVED. Jitter is applied as a per-chunk delay, but a chunk is never
scheduled before the one ahead of it: reordering a TCP stream would corrupt it rather
than simulate a network. So jitter widens the gap between arrivals without shuffling
them, which is exactly what a jitter buffer is built to absorb.
"""
import argparse
import asyncio
import random
import sys


async def pump(reader, writer, delay_s, jitter_s, rng):
    """Copy reader -> writer, holding each chunk for delay +/- jitter.

    READING AND DELIVERY ARE SEPARATE TASKS. Doing both in one loop -- read, sleep,
    write, read again -- does not model a delay, it models a stall: bytes arriving
    during the sleep are not even read until the previous chunk has been delivered, and
    then wait a further full delay. At 250ms one way, chunks arriving at 0 and 50ms
    would leave at 250 and 500ms instead of 250 and 300ms, so the relay silently
    becomes a bandwidth limiter and the latency numbers measured through it are its own
    batching rather than the engine's behaviour.

    The reader timestamps each chunk on arrival and queues it; the writer sleeps until
    each chunk is due. Ordering still holds, because the queue is FIFO and each due
    time is clamped to at least the previous one.
    """
    loop = asyncio.get_running_loop()
    q = asyncio.Queue()
    next_ok = 0.0

    async def read_side():
        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                nonlocal next_ok
                d = delay_s
                if jitter_s:
                    d += rng.uniform(-jitter_s, jitter_s)
                    if d < 0:
                        d = 0.0
                # Stamp when it ARRIVED plus its delay -- not when we get round to it.
                due = max(loop.time() + d, next_ok)
                next_ok = due
                await q.put((due, data))
        finally:
            await q.put(None)

    async def write_side():
        while True:
            item = await q.get()
            if item is None:
                break
            due, data = item
            wait = due - loop.time()
            if wait > 0:
                await asyncio.sleep(wait)
            writer.write(data)
            await writer.drain()

    try:
        await asyncio.gather(read_side(), write_side())
    except (ConnectionResetError, BrokenPipeError, asyncio.IncompleteReadError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def handle(local_r, local_w, host, port, delay_s, jitter_s, rng, stats):
    try:
        remote_r, remote_w = await asyncio.open_connection(host, port)
    except OSError as e:
        print(f"netdelay: cannot reach {host}:{port} -- {e}", file=sys.stderr)
        local_w.close()
        return
    stats["conns"] += 1
    await asyncio.gather(
        pump(local_r, remote_w, delay_s, jitter_s, rng),
        pump(remote_r, local_w, delay_s, jitter_s, rng),
    )


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", type=int, required=True)
    ap.add_argument("--to", required=True, help="host:port")
    ap.add_argument("--rtt", type=float, default=0.0,
                    help="round trip ms; half is applied in each direction")
    ap.add_argument("--jitter", type=float, default=0.0,
                    help="+/- ms applied per chunk, per direction (order preserved)")
    ap.add_argument("--seed", type=int, default=1, help="jitter RNG seed")
    a = ap.parse_args()

    host, _, port = a.to.rpartition(":")
    delay_s = (a.rtt / 2.0) / 1000.0        # RTT is a round trip: half in each direction
    # Jitter is documented as +/- per direction, so it is NOT halved -- doing so made
    # --jitter 30 behave as +/-15ms and quietly understate what was being tested.
    jitter_s = a.jitter / 1000.0
    rng = random.Random(a.seed)
    stats = {"conns": 0}

    server = await asyncio.start_server(
        lambda r, w: handle(r, w, host, int(port), delay_s, jitter_s, rng, stats),
        "127.0.0.1", a.listen)
    print(f"netdelay: 127.0.0.1:{a.listen} -> {a.to}  rtt={a.rtt}ms jitter=+/-{a.jitter}ms",
          flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
