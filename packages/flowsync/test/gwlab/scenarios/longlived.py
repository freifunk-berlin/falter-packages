"""Connections that outlive the established timeout: one per gateway pair,
talking all the time, for 2.2 established timeouts (4.5 hours in production).
At a scale of 30 that is nine minutes, and every timer but one (TCP close,
10 s, held at 1 s) still divides evenly. A gateway that only holds an idle
copy of such a connection must not cut it."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

SCALE = 30
FLOWS = grid(traffic=["tcp_long"], client="A", server="B", fwd=GW, rev=GW)
