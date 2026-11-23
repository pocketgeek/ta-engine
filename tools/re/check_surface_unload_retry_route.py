#!/usr/bin/env python3
"""Run one native sea-unload mission through retry, retail search and mover.

The native GROUND_UNLOAD dispatcher creates the replacement exact-site circle
after a same-trip out-of-range retry. Retail's real route search and mover then
use that same carrier, passenger, mission, navigator and controller in the same
Unicorn address space. World supplies the comparison grade plane and movement
trace; host-controlled placement, effects, COB and the path-request scheduler
remain fixture boundaries.
"""
import argparse

from check_surface_unload_route import check_variant


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('world_binary', nargs='?', default='build-o2/transport_test')
    parser.add_argument('--steps', type=int, default=1000,
                        help='paired physical mover steps after the remote retry position (1..1000)')
    args = parser.parse_args()
    if not 1 <= args.steps <= 1000:
        parser.error('--steps must be between 1 and 1000')

    check_variant(args.world_binary, 8, args.steps, integrated_retry=True)


if __name__ == '__main__':
    main()
