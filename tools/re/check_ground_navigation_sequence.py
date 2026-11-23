#!/usr/bin/env python3
"""Compare independent waypoint advancement, scans, steering and commitment.

Both implementations receive one authored partial route per unit and retain
their own evolving route and movement state. Goal acceptance stays false,
formation is disabled, and search requests are queued without running workers.
Original navigator advancement, local scans, movement and height execute.
"""
from check_ground_stop_sequence import main


if __name__=='__main__': main(navigation=True,description=__doc__)
