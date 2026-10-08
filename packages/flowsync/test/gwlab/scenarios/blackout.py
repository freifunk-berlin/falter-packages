"""gw2 is cut off from the sync from before the first flow until shortly after the last one started: it
misses the start of every flow. Whatever the implementation does to repair a
lost announcement has to work as fast as its sync does."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

FLOWS = grid(traffic=["tcp_talk", "udp_stream", "udp_rr"], client="A", server="B", fwd=GW, rev=GW)
EVENTS = [dict(at=-10, do="sync_blackout", gw="gw2", seconds=30)]
