"""Connections that fall silent: talk, ten minutes of nothing, the server
speaks; ten more minutes, the client speaks. Over every gateway pair. One
gateway keeps an established connection for two hours; two must not lose it
in ten minutes."""
from ..scenario import grid
from .gw5 import FLEETS, GW, TOPOLOGY  # noqa: F401

FLOWS = grid(traffic=["tcp_idle"], client="A", server="B", fwd=GW, rev=GW)
