"""The exit flaps: a flow's forward path leaves through the next gateway and
comes back, twice, while its replies keep arriving through the first. That is
a gateway whose BGP session goes down (its mesh traffic leaves inside GRE
through another gateway, whose uplink learns and announces the flows) and
comes up again (the gateway must find its own flows again on their next
packet out while the peer's announcements of them fade).

  start        out and back via gw N                symmetric
  1/5 in       out via N+1, back via N              BGP down
  2/5 in       out and back via N                   BGP up
  3/5 in       out via N+1, back via N              down again
  4/5 in       out and back via N                   up again

Every client to every server, starting on each of the five gateways."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

FLOWS = [f for f in grid(traffic=["tcp_talk", "udp_stream"], client="AB", server="AB", fwd=GW, rev=GW)
         if f["fwd"] == f["rev"]]
EVENTS = [dict(at=100, do="reroute", leg="fwd"),      # production time, as the traffic
          dict(at=200, do="reroute", leg="fwd", step=-1),
          dict(at=300, do="reroute", leg="fwd"),
          dict(at=400, do="reroute", leg="fwd", step=-1)]
