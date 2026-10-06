"""Measurements: what a gateway costs and what it sustains. One flow at a
time, nothing else running; no link latency, segmentation offload off (packets
as a router sees them). Each traffic over one gateway and over two; a third
gateway only receives the sync, as most of a fleet does for any flow.

  flood_up, flood_down   packets per second one core pushes through the path
  bulk                   Mbit/s of one TCP download
  newflows               new flows per second the return gateway knows in time

The numbers compare implementations on the same host; they are not a
router's."""
from ..scenario import grid

ALONE = True

TOPOLOGY = dict(
    gateways=dict(gw1=dict(mesh=0, uplink=0), gw2=dict(mesh=0, uplink=0), gw3=dict(mesh=0, uplink=0)),
    clients=dict(A=0),
    servers=dict(A=0),
    offloads=False,
)
GW = list(TOPOLOGY["gateways"])
FLEETS = dict(
    strict={g: dict(bypass=False) for g in GW},
    offload={g: dict(bypass=False, offload=True) for g in GW},
)
FLOWS = grid(traffic=["flood_up", "flood_down", "bulk", "newflows"],
             client="A", server="A", fwd=["gw1"], rev=["gw1", "gw2"])
