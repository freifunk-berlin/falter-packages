"""How fast is the sync, seen from outside? New flows over every gateway pair
to a server right at the Internet side of the gateways (distance 0), which
holds its first answer back by 0 to 1000 ms. The sync travels the same uplinks
as the answer, so the delay is exactly the time the gateways have. The report
has, per delay, how many answers passed, and from which delay on all did."""
from ..scenario import grid
from .gw5 import GW, TOPOLOGY as GW5

TOPOLOGY = dict(GW5, servers=dict(A=0))
FLEETS = dict(strict={g: dict(bypass=False) for g in GW})
FLOWS = grid(traffic=["ladder"], client="A", server="A", fwd=GW, rev=GW)
