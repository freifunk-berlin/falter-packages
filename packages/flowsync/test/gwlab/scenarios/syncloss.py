"""gw2's sync path loses 30 % of its packets, both ways, all the time. The
sync latency is measured under that loss (with answers held back up to 4.5 s, 45 s in production),
and the flows are judged against what the implementation achieves: one that
repeats what it announced gets there late, one that says everything once
leaves flows without state for good."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY as GW5  # noqa: F401

TOPOLOGY = dict(GW5, sync=dict(gw2=dict(loss=30)),
                ladder_ms=[0, 5, 50, 500, 1000, 2000, 3000, 4000, 4500])
FLOWS = grid(traffic=["tcp_short", "tcp_talk", "udp_rr", "udp_stream"],
             client="A", server="B", fwd=GW, rev=GW)
