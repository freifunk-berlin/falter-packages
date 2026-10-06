"""The paths move while the flows run: 20 s in, every flow's return path goes
to the next gateway; 40 s in, its forward path does. The gateways a flow moves
to never saw its start. Client B to server B over every gateway pair."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

FLOWS = grid(traffic=["tcp_talk", "udp_stream"], client="B", server="B", fwd=GW, rev=GW)
EVENTS = [dict(at=20, do="reroute", leg="rev"),
          dict(at=40, do="reroute", leg="fwd")]
