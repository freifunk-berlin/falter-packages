"""flowsync as it was on conntrack (branch flowsync-conntrack): announces
conntrack entries to the peers, which create copies in their conntrack tables.
Kept to compare against.

Options: bin=<a flowsync binary built from that branch> (required: ../src is
the BPF daemon now)."""
import os
import sys

from . import TEST, Impl, stop_wrapped, ctfw

INTERVAL, ELEMENT_TIMEOUT = 30, 90      # its own timers as shipped (seconds); the lab scales them


class IMPL(Impl):
    def __init__(self, opts):
        super().__init__(opts)
        if "bin" not in opts:
            raise SystemExit("flowsync_conntrack: --set bin=<flowsync built from branch "
                             "flowsync-conntrack> is required")
        self.bin = os.path.abspath(opts["bin"])
        self.procs = {}

    def install(self, gw):
        ctfw.install(gw)

    def timers(self, t):
        interval = t.s("flowsync interval", INTERVAL)
        # the daemon insists on three intervals, also where the scale holds both at 1 s
        return interval, max(t.s("flowsync element_timeout", ELEMENT_TIMEOUT), 3 * interval)

    def broken(self, t):
        interval, element = self.timers(t)
        c = t.conntrack()
        # the daemon's own startup check: a copy that carries traffic must live
        # on its packets, not be cut back to element_timeout every round
        return ["flowsync's refresh (element_timeout + interval, 120 s) is no shorter than %s"
                % what for what, v in (("the UDP stream timeout (180 s)", c["udp_timeout_stream"]),
                                       ("TCP unacknowledged (300 s)", c["tcp_timeout_unacknowledged"]))
                if element + interval >= v]

    def start(self, gw):
        argv = [sys.executable, os.path.join(TEST, "ptyrun.py"), self.bin,
                "-b", gw.addr4, "-I", gw.uplink_if]
        for p in gw.peers4:
            argv += ["-e", p]
        interval, element = self.timers(gw.timers)
        argv += ["-x", gw.client_net, "-D", gw.mesh_net, "-i", interval, "-t", element,
                 "-s", os.path.join(gw.dir, "status"), "run"]
        self.procs[gw.name] = gw.spawn(*argv)

    def stop(self, gw):
        p = self.procs.pop(gw.name, None)
        if p:
            stop_wrapped(p)

    def lose_state(self, gw):
        ctfw.flush(gw)
