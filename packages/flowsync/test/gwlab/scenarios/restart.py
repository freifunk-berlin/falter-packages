"""The sync software on gw2 is stopped and started again a third into the
run (an update, a crash and respawn). Flows through gw2 must go on and no
connection may break."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

FLOWS = grid(traffic=["tcp_talk", "udp_stream"], client="A", server="B", fwd=GW, rev=GW)
EVENTS = [dict(at=200, do="restart", gw="gw2")]
