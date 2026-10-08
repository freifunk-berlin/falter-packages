"""What a scenario works with: the topology, the gateways, flows, checks and
timers of one run."""
import os
import threading
import time

from .flow import Bulk, Flow
from .topo import Topology


class Loop:
    """fn() every period seconds, n times, in the background; a failing fn
    fails the scenario (once), or traffic that never flowed could pass checks
    that something is gone"""

    def __init__(self, n, period, fn, checks=None):
        self.stopped = False
        self.checks = checks
        self.failed = False
        self.t = threading.Thread(target=self._run, args=(n, period, fn), daemon=True)
        self.t.start()

    def _run(self, n, period, fn):
        for _ in range(n):
            if self.stopped:
                return
            try:
                fn()
            except Exception as e:
                if self.checks and not self.failed:
                    self.failed = True
                    self.checks.fail("background traffic: %s" % e)
            time.sleep(period)

    def wait(self):
        self.t.join()

    def stop(self):
        self.stopped = True
        self.t.join()


class Env:
    # scaled timers: refresh interval, element_timeout, and the lifetimes of
    # local flows after their last packet out (production: 180, 7440, 120,
    # 120, 600 s)
    I = 3
    E = 9
    timers = {"udp": 12, "tcp": 40, "tcp_syn": 10, "tcp_close": 8, "other": 15}

    def __init__(self, gateways, paths, workdir, checks, topo=None):
        self.flowsync = paths["flowsync"]
        self.bpf_object = paths["bpf_object"]
        # DPTEST_BYPASS=1: every daemon runs with --bypass
        self.bypass = os.environ.get("DPTEST_BYPASS", "") not in ("", "0")
        self.probe_path = paths["probe"]
        self.ptyrun = paths["ptyrun"]
        self.python = paths["python"]
        self.dir = workdir
        self.c = checks
        self.loops = []
        if topo is not None:
            topo.bind(self)
            self.topo = topo
        else:
            self.topo = Topology(self, gateways)
        self.g = self.topo.g
        self.cl = self.topo.cl
        self.sv = self.topo.sv
        self.hub = self.topo.hub

    def next_id(self):
        self.topo.next_flow += 1
        return self.topo.next_flow

    # -------------------------------------------------------- building
    def flow(self, proto="udp", fw=None, rev=None, **kw):
        return Flow(self, proto, fw, rev, **kw)

    def bulk(self, n, **kw):
        return Bulk(self, n, **kw)

    def start(self, *opts, wait=True, debug=True):
        """flowsync on every gateway with the same extra options"""
        for g in self.g:
            g.start(*opts, debug=debug)
        if wait:
            self.c.wait_for("daemons up", 8, lambda: all(g.up() for g in self.g))

    def loop(self, n, period, fn):
        lp = Loop(n, period, fn, self.c)
        self.loops.append(lp)
        return lp

    def spawn(self, node, *args, out=None):
        """a probe.py command in the background, e.g. a server"""
        f = open(out, "w") if out else None
        p = node.spawn(self.python, self.probe_path, *args, stdout=f)
        if f:
            f.close()
        return p

    def probe(self, node, *args, check=True):
        return node.run(self.python, self.probe_path, *args, check=check)

    # --------------------------------------------------------- checks
    def check(self, *a):
        return self.c.check(*a)

    def true(self, *a):
        return self.c.true(*a)

    def ok(self, text):
        self.c.ok(text)

    def wait_for(self, desc, timeout, fn, step=0.25):
        return self.c.wait_for(desc, timeout, fn, step)

    def hold(self, desc, secs, fn, step=1.0):
        return self.c.hold(desc, secs, fn, step)

    def wait_st(self, desc, g, key, n):
        """a status counter, which reaches the file at the daemon's next tick"""
        return self.c.wait_for(desc, 2 * self.I + 2, lambda: (g.st(key) or 0) >= n)

    @staticmethod
    def sleep(secs):
        time.sleep(secs)

    def close(self, keep=False):
        """end the scenario: reset the topology for the next one (keep) or
        tear it down"""
        for lp in self.loops:
            lp.stopped = True
        for lp in self.loops:
            lp.t.join(5)
        if keep:
            self.topo.reset()
        else:
            self.topo.close()
