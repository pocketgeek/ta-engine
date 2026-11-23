#!/usr/bin/env python3
"""Compare active retail/World movement over controlled navigation segments.

Segments and movement mode are authored inputs, with scans and formation
disabled. Both implementations retain their own evolving movement, occupancy,
and height state. This does not verify route generation or route advancement.
"""
from check_ground_stop_sequence import main


if __name__=='__main__': main(travel=True,description=__doc__)
