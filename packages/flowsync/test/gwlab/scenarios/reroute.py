"""The paths move while the flows run: a third in, every flow's return path goes
to the next gateway; two thirds in, its forward path does. The gateways a flow moves
to never saw its start. Client B to server B over every gateway pair."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

FLOWS = grid(traffic=["tcp_talk", "udp_stream"], client="B", server="B", fwd=GW, rev=GW)
EVENTS = [dict(at=200, do="reroute", leg="rev"),       # production time, as the traffic
          dict(at=400, do="reroute", leg="fwd")]
