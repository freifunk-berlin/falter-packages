"""Every client talks to every server over every pair of gateways (5 symmetric,
20 asymmetric), all flows at once. A flow passes if it meets expect.py."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

FLOWS = grid(traffic=["tcp_short", "tcp_talk", "udp_rr", "udp_stream"],
             client="AB", server="AB", fwd=GW, rev=GW)          # 400 flows per lab
