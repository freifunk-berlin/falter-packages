"""gw2 is cut off from the sync from before the first flow until 2 s in: it
misses the start of every flow. Whatever the implementation does to repair a
lost announcement has to have worked by the end of the event's grace."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

FLOWS = grid(traffic=["tcp_talk", "udp_stream", "udp_rr"], client="A", server="B", fwd=GW, rev=GW)
EVENTS = [dict(at=-1, do="sync_blackout", gw="gw2", seconds=3)]
