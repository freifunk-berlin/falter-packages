"""flowsync, the conntrack-injection design: announces conntrack entries to
the peers, which create copies in their conntrack tables.

Options: bin=<flowsync binary> (default: ../src/flowsync)."""
import os
import signal
import sys
import time

from . import TEST, Impl, children, ctfw
from ..ns import kill

INTERVAL, ELEMENT_TIMEOUT = 3, 9        # scaled like the conntrack timeouts (production 30 / 90 s)


class IMPL(Impl):
    def __init__(self, opts):
        super().__init__(opts)
        self.bin = os.path.abspath(opts.get("bin", os.path.join(TEST, "..", "src", "flowsync")))
        self.procs = {}

    def install(self, gw):
        ctfw.install(gw)

    def start(self, gw):
        argv = [sys.executable, os.path.join(TEST, "ptyrun.py"), self.bin,
                "-b", gw.addr4, "-I", gw.uplink_if]
        for p in gw.peers4:
            argv += ["-e", p]
        argv += ["-x", gw.client_net, "-D", gw.mesh_net, "-i", INTERVAL, "-t", ELEMENT_TIMEOUT,
                 "-s", os.path.join(gw.dir, "status"), "run"]
        self.procs[gw.name] = gw.spawn(*argv)

    def stop(self, gw):
        p = self.procs.pop(gw.name, None)
        if p:
            # the daemon is ptyrun's child, in a session of its own
            for pid in children(p.pid):
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            for _ in range(40):
                if not children(p.pid):
                    break
                time.sleep(0.05)
            kill(p)

    def lose_state(self, gw):
        ctfw.flush(gw)
