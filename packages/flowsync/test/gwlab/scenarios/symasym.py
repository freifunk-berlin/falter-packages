"""A flow that starts on one gateway, is split over two and ends on one again,
twice: the egress moves first and the ingress follows, then the ingress moves
first and the egress follows.

  start        out and back via gw N                symmetric
  1/5 in       out via N+1, back via N              asymmetric: egress moved
  2/5 in       out and back via N+1                 symmetric again
  3/5 in       out via N+1, back via N+2            asymmetric: ingress moved
  4/5 in       out and back via N+2                 symmetric again

Every client to every server, starting on each of the five gateways. The
gateway a path moves to never saw the flow start."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

FLOWS = [f for f in grid(traffic=["tcp_talk", "udp_stream"], client="AB", server="AB", fwd=GW, rev=GW)
         if f["fwd"] == f["rev"]]
EVENTS = [dict(at=100, do="reroute", leg="fwd"),      # production time, as the traffic
          dict(at=200, do="reroute", leg="rev"),
          dict(at=300, do="reroute", leg="rev"),
          dict(at=400, do="reroute", leg="fwd")]
