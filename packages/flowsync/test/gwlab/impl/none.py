"""No sync at all: every gateway is a stateful firewall on its own. The
baseline: symmetric flows must work, asymmetric ones cannot."""
from . import Impl, ctfw


class IMPL(Impl):
    def install(self, gw):
        ctfw.install(gw)

    def lose_state(self, gw):
        ctfw.flush(gw)
