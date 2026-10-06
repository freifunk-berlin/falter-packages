"""gw2 loses every flow it knows 20 s in (a flush, a reboot): the flows through
it have to be back within the event's grace, and no connection may break."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

FLOWS = grid(traffic=["tcp_talk", "udp_stream"], client="A", server="B", fwd=GW, rev=GW)
EVENTS = [dict(at=20, do="lose_state", gw="gw2")]
