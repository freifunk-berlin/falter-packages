"""gw2's uplink device is deleted and created again 20 s in (same name and
addresses, new ifindex: what happens to a VLAN uplink on a network restart).
Flows through gw2 lose packets while it is gone; afterwards traffic and sync
have to work again without anybody restarting anything."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

FLOWS = grid(traffic=["tcp_talk", "udp_stream"], client="A", server="B", fwd=GW, rev=GW)
EVENTS = [dict(at=20, do="uplink_recreate", gw="gw2")]
