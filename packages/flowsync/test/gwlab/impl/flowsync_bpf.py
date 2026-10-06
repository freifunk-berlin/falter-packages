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
from ..ns import kill

MARK = 0x01000000
INTERVAL, ELEMENT_TIMEOUT = 3, 9
# lifetimes of a flow after its last packet out, scaled (production 180, 7440, 120, 120, 600 s)
TIMEOUTS = {"udp": 12, "tcp": 40, "tcp-syn": 10, "tcp-close": 8, "other": 15}

RULES = """
table inet fw {
	counter fwd_ack {}
	counter fwd_rej {}
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
            argv += ["--%s-timeout" % k, v]
        return argv

    def install(self, gw):
        bypass = ("\t\tmeta nfproto ipv6 tcp flags & ack == ack limit rate 5000/second "
                  "burst 2500 packets counter name fwd_ack accept\n"
                  "\t\tmeta nfproto ipv6 tcp flags & rst == rst limit rate 1000/second "
                  "burst 500 packets accept") if gw.policy["bypass"] else ""
        gw.node.nft(RULES % dict(mark=MARK, bypass=bypass))

    def start(self, gw):
        argv = [sys.executable, os.path.join(TEST, "ptyrun.py"), self.bin,
                "-b", gw.addr4, "-I", gw.uplink_if]
        for p in gw.peers4:
            argv += ["-e", p]
        argv += ["-x", gw.client_net, "-D", gw.mesh_net, "-i", INTERVAL, "-t", ELEMENT_TIMEOUT,
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
