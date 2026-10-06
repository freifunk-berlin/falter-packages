"""Connections that outlive the established timeout: one per gateway pair,
talking all the time, for 2.2 established timeouts (4.5 hours in production).
At a scale of 100 that is under three minutes; the timers that matter here
(TCP established and unacknowledged) keep their ratio, shorter ones are held
at 1 s. A gateway that only holds an idle copy of such a connection must not
cut it."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

SCALE = 100
FLOWS = grid(traffic=["tcp_long"], client="A", server="B", fwd=GW, rev=GW)
