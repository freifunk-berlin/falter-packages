"""flowsync-bpf, the conntrack-bypass design (branch flowsync-bpf): forwarded
IPv6 is not tracked; tc programs on the uplink keep the flows in BPF maps and
mark packets of known flows, the firewall accepts the mark, the daemon syncs
the maps between the gateways.

Needs real root (BPF cannot be loaded in a user namespace): run it in a VM.
No flow offloading. Options: bin=<flowsync binary>, bpf_object=<flowsync.o>."""
import os
import signal
import sys
import time

from . import TEST, Impl, children
from .ctfw import BYPASS, BYPASSED_SET
from ..ns import kill

MARK = 0x01000000
# its own timers as shipped (seconds); the lab scales them
INTERVAL, ELEMENT_TIMEOUT = 30, 90
# lifetimes of a flow after its last packet out
TIMEOUTS = {"udp": 180, "tcp": 7440, "tcp-syn": 120, "tcp-close": 120, "other": 600}

RULES = """
table inet fw {
	counter fwd_ack {}
	counter fwd_rej {}
%(set)s
	chain forward {
		type filter hook forward priority 0; policy drop;
		meta nfproto ipv6 meta mark & 0x%(mark)08x == 0x%(mark)08x accept comment "flowsync"
		ct state established,related accept
		iifname "mesh0" accept
%(bypass)s
		counter name fwd_rej
		meta l4proto tcp reject with tcp reset
		reject
	}
}
table inet flowsync {
	chain prerouting {
		type filter hook prerouting priority raw; policy accept;
		meta nfproto ipv6 fib daddr type unicast notrack
	}
	chain defrag {
		ct state untracked accept
	}
}
"""


class IMPL(Impl):
    needs_root = True
    offload = False

    def __init__(self, opts):
        super().__init__(opts)
        self.bin = os.path.abspath(opts.get("bin", os.path.join(TEST, "..", "src", "flowsync")))
        self.obj = os.path.abspath(opts.get("bpf_object", os.path.join(os.path.dirname(self.bin),
                                                                       "flowsync.o")))
        self.procs = {}

    def base(self, gw):
        argv = ["-U", gw.uplink_if, "--fw-table", "fw",
                "--pin-dir", "/sys/fs/bpf/gwlab-%d-%s" % (os.getpid(), gw.name),
                "--bpf-object", self.obj]
        for k, v in TIMEOUTS.items():
            argv += ["--%s-timeout" % k, gw.timers.s("flowsync-bpf %s timeout" % k, v)]
        return argv

    def install(self, gw):
        gw.node.nft(RULES % dict(mark=MARK, set=BYPASSED_SET,
                                 bypass=BYPASS if gw.policy["bypass"] else ""))

    def start(self, gw):
        argv = [sys.executable, os.path.join(TEST, "ptyrun.py"), self.bin,
                "-b", gw.addr4, "-I", gw.uplink_if]
        for p in gw.peers4:
            argv += ["-e", p]
        interval = gw.timers.s("flowsync-bpf interval", INTERVAL)
        # the daemon insists on three intervals, also where the scale holds both at 1 s
        element = max(gw.timers.s("flowsync-bpf element_timeout", ELEMENT_TIMEOUT), 3 * interval)
        argv += ["-x", gw.client_net, "-D", gw.mesh_net, "-i", interval, "-t", element,
                 "-s", os.path.join(gw.dir, "status")] + self.base(gw) + ["run"]
        self.procs[gw.name] = gw.spawn(*argv)

    def stop(self, gw):
        p = self.procs.pop(gw.name, None)
        if p:
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
        # programs and maps outlive the daemon: take them down with the lab
        gw.node.run(self.bin, *map(str, self.base(gw)), "detach", check=False)

    def lose_state(self, gw):
        """as after a reboot: programs off, maps gone, daemon started again"""
        self.stop(gw)
        self.start(gw)
