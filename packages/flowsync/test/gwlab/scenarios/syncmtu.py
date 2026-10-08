"""gw2's sync path drops every packet above 1280 bytes (a tunnel in between,
broken path MTU discovery). The measurement before the flows sends single
announcements, which fit; the 100 flows starting together are what makes an
implementation fill its datagrams."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY as GW5  # noqa: F401

TOPOLOGY = dict(GW5, sync=dict(gw2=dict(mtu=1280)))
FLOWS = grid(traffic=["tcp_short", "tcp_talk", "udp_rr", "udp_stream"],
             client="A", server="B", fwd=GW, rev=GW)
