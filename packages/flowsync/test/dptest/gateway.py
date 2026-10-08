"""A gateway: an fw4-like ruleset, the flowsync daemon with its tc programs
on the uplink (wan0), flow table queries and controls on the sync path."""
import os
import re
import sys
import time

from .ns import kill

PORT = 3994
PREFIX = "2001:db8:100::/44"        # synced client prefix
XDST = "2001:db8::/32"              # the mesh: never synced as a server
SERVER_NET = "2a00:1450:4001::/48"  # where the test's servers live
MARK = 0x01000000
MSS = 1416                          # the gateways' clamp: what fits their GRE tunnels

# what the daemon installs at its start (fw.c), without the alive element:
# the daemon writes that one, and until it does conntrack tracks what leaves
OWN_TABLE = """
table ip6 flowsync {
	set alive {
		type iface_index
		flags timeout
	}
	chain prerouting {
		type filter hook prerouting priority raw; policy accept;
		meta mark & 0x01000000 == 0x01000000 notrack accept
		fib daddr oif @alive notrack
	}
	chain defrag {
		ct state untracked accept
	}
}
"""


def children(pid):
    """pids whose parent is pid"""
    out = []
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % d) as f:
                st = f.read()
            if int(st[st.rindex(")") + 2:].split()[1]) == pid:
                out.append(int(d))
        except (OSError, ValueError):
            pass
    return out


class Entry:
    """one flow in a gateway's tables, as `flowsync flow` prints it"""

    def __init__(self, text):
        self.raw = " | ".join(text.split("\n"))
        m = re.search(r"local left=(\d+) age=(\d+) flags=(\d+)", text)
        self.local_left = int(m.group(1)) if m else 0
        self.age = int(m.group(2)) if m else None
        self.flags = int(m.group(3)) if m else 0
        m = re.search(r"remote left=(\d+)", text)
        self.remote_left = int(m.group(1)) if m else 0
        if "local" not in text or "remote" not in text:
            # a failed query is not an absent flow (wait_for retries it)
            raise RuntimeError("flowsync flow: %s" % (text.strip() or "no output"))

    @property
    def local(self):
        """alive in the local table: this gateway forwarded it out"""
        return self.local_left > 0

    @property
    def remote(self):
        """alive in the remote table: a peer announced it"""
        return self.remote_left > 0

    @property
    def alive(self):
        return self.local or self.remote

    def __str__(self):
        return self.raw


class Gateway:
    def __init__(self, env, idx, node):
        self.i = idx
        self.node = node
        self.addr = "10.0.0.%d" % (idx + 1)
        self.proc = None
        self.opts = []
        self.pin = "/sys/fs/bpf/dptest-%d-g%d" % (os.getpid(), idx)
        self.bind(env)

    def bind(self, env):
        """attach to a scenario run: its timers, binaries and log directory"""
        self.env = env
        self.status = os.path.join(env.dir, "g%d.status" % self.i)
        self.logpath = os.path.join(env.dir, "g%d.log" % self.i)

    def __repr__(self):
        return "g%d" % self.i

    # ------------------------------------------------------------- setup
    def setup(self):
        self.node.sysctl(**{"net.ipv6.conf.all.forwarding": 1})
        self.node.nft(self.ruleset())
        self.node.nft(OWN_TABLE)

    def ruleset(self, accept_rule=True):
        """fw4 as on the gateways: the MSS clamp on every forwarded SYN first
        (bbb-configs' chain-prepend include), forward policy reject,
        established accept (conntrack sees nothing forwarded once the daemon's
        table is in), the mesh (mesh0, or a tunnel over it) may go anywhere. No stateless budget for TCP
        segments with ACK or RST: nothing here may pass without a flow. The
        accept rule for marked packets is the one the package ships as an
        fw4 include."""
        return """
table inet fw {
	counter fwd_mark {}
	counter fwd_est {}
	counter fwd_rej {}
	chain forward {
		type filter hook forward priority 0; policy drop;
		meta nfproto ipv6 tcp flags syn tcp option maxseg size set %(mss)d
%(accept)s
		ct state established,related counter name fwd_est accept
		iifname { "mesh0", "tun0", "tun1", "tun2" } accept
		counter name fwd_rej
		meta l4proto tcp reject with tcp reset
		reject
	}
}
table inet sync {
	chain in {
		type filter hook input priority -10;
	}
}
""" % {
            "mss": MSS,
            "accept": ('\t\tmeta nfproto ipv6 meta mark & 0x%08x == 0x%08x counter name fwd_mark '
                       'accept comment "flowsync"' % (MARK, MARK)) if accept_rule else "",
        }

    # ------------------------------------------------------------ daemon
    def base_opts(self):
        """what every flowsync command on this gateway needs: where the maps,
        the uplink and the firewall's table are"""
        return ["-U", "wan0", "--fw-table", "fw", "--pin-dir", self.pin,
                "--bpf-object", self.env.bpf_object]

    def run_opts(self):
        """what the daemon gets on top: the timeouts (the flow commands take
        them from the running programs)"""
        t = self.env.timers
        return ["--udp-timeout", t["udp"], "--tcp-timeout", t["tcp"],
                "--tcp-syn-timeout", t["tcp_syn"], "--tcp-close-timeout", t["tcp_close"],
                "--other-timeout", t["other"]] + (["--bypass"] if self.env.bypass else [])

    def start(self, *opts, debug=True, key=None):
        """start flowsync with the standard options plus opts (later options win);
        debug=False for scenarios with thousands of records a second; key: its
        FLOWSYNC_KEY (as the init script passes it), None for none"""
        env = self.env
        if self.proc:
            self.stop()
        try:
            os.unlink(self.status)
        except FileNotFoundError:
            pass
        self.opts = [str(o) for o in opts]
        self.debug = debug
        peers = []
        for g in env.g:
            if g is not self:
                peers += ["-e", g.addr]
        self.key = key
        argv = (["env", "FLOWSYNC_KEY=" + key] if key else []) + [
            sys.executable, env.ptyrun, env.flowsync, "-b", self.addr, "-I", "eth0"] + peers + [
            "-x", PREFIX, "-D", XDST, "-i", env.I, "-t", env.E, "-s", self.status
        ] + self.base_opts() + self.run_opts() + (["-d"] if debug else []) + self.opts + ["run"]
        log = open(self.logpath, "a")
        self.proc = self.node.spawn(*argv, stdout=log, stderr=log)
        log.close()

    def stop(self):
        """SIGTERM to flowsync itself (ptyrun's child, in a session of its own),
        so it shuts down cleanly; then the wrapper. The programs stay on the
        uplink and the maps pinned."""
        if self.proc:
            for pid in children(self.proc.pid):
                try:
                    os.kill(pid, 15)
                except ProcessLookupError:
                    pass
            for _ in range(40):
                if not children(self.proc.pid):
                    break
                time.sleep(0.05)
            kill(self.proc)
            if self.proc in self.node.spawned:
                self.node.spawned.remove(self.proc)
            self.proc = None

    def restart(self, *opts):
        self.stop()
        self.start(*(opts or self.opts), debug=getattr(self, "debug", True),
                   key=getattr(self, "key", None))

    def cmd(self, *args, check=False):
        """a flowsync subcommand on this gateway's maps"""
        return self.node.run(self.env.flowsync, *self.base_opts(), *args, check=check)

    def detach(self):
        """programs off the uplink, maps gone, rules gone: as after a reboot"""
        self.cmd("detach")

    def up(self):
        return os.path.exists(self.status)

    def st(self, key):
        """a counter or gauge from the status file; None if missing"""
        try:
            with open(self.status) as f:
                for line in f:
                    k, _, v = line.partition(" ")
                    if k == key:
                        return int(v.split()[0])
        except (FileNotFoundError, ValueError):
            pass
        return None

    def status_line(self, prefix):
        try:
            with open(self.status) as f:
                for line in f:
                    if line.startswith(prefix):
                        return line.strip()
        except FileNotFoundError:
            pass
        return ""

    def log(self):
        try:
            with open(self.logpath) as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def tick(self, timeout=None):
        """wait until the daemon has just written its status, plus a moment:
        it does that at its tick, right after starting a round (the status
        shows the rounds completed before it; the new one runs now)"""
        timeout = timeout or 3 * self.env.I + 5
        m0 = os.stat(self.status).st_mtime_ns if self.up() else 0
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            if self.up() and os.stat(self.status).st_mtime_ns != m0:
                time.sleep(0.3)
                return True
            time.sleep(0.05)
        return False

    # ------------------------------------------------------- flow tables
    def ft(self, flow, proto=None, sport=None):
        return self.ft_tuple(proto or flow.proto, flow.c, flow.cport, flow.s, sport or flow.sport)

    def ft_tuple(self, proto, c, cport, s, sport):
        return Entry(self.cmd("flow", c, cport, s, sport, proto))

    def flows(self, which):
        """lines of `flowsync flows local|remote`"""
        out = self.cmd("flows", which)
        return [ln for ln in out.split("\n") if ln.startswith(which)]

    def ct_count(self):
        """conntrack entries on the gateway (the sync socket's own included)"""
        return int(self.node.read("/proc/sys/net/netfilter/nf_conntrack_count") or 0)

    def ifindex(self, dev="wan0"):
        out = self.node.run("ip", "-o", "link", "show", dev, check=False)
        return int(out.split(":")[0]) if out and out[0].isdigit() else 0

    def alive(self):
        """the alive element for the current uplink device is in the daemon's
        set: what leaves through it is untracked"""
        out = self.node.run("nft", "list", "set", "ip6", "flowsync", "alive", check=False)
        # nft prints the element as the device's name while the device exists,
        # as its number once it is gone
        return re.search(r'(?:"?wan0"?|\b%d) timeout \d' % self.ifindex(), out) is not None

    def fwc(self, name):
        """a forward chain counter: mark (accepted on a flow), est (accepted
        by conntrack), rej"""
        out = self.node.run("nft", "list", "counter", "inet", "fw", "fwd_" + name, check=False)
        m = re.search(r"packets (\d+)", out)
        return int(m.group(1)) if m else 0

    # ------------------------------------------------------ sync control
    def _sync_rule(self, rule):
        self.node.run("nft", "add", "rule", "inet", "sync", "in", *rule.split())

    def block_sync(self, src=None):
        """drop sync datagrams from one peer (a Gateway) or from all"""
        self._sync_rule(("ip saddr %s " % src.addr if src else "") + "udp dport %d drop" % PORT)

    def loss(self, every=3):
        """drop every n-th record datagram (heartbeats are 12 bytes of UDP)"""
        self._sync_rule("udp dport %d udp length gt 12 numgen inc mod %d == 0 drop" % (PORT, every))

    def mtu_blackhole(self, size=1280):
        """a path that silently loses sync packets above size bytes"""
        self._sync_rule("udp dport %d meta length gt %d drop" % (PORT, size))

    def unblock_sync(self):
        self.node.run("nft", "flush", "chain", "inet", "sync", "in")

    def reset(self):
        """back to the state after setup(), for the next scenario: no daemon,
        no programs, empty maps, the rules as built"""
        self.stop()
        self.detach()
        self.node.sh("nft delete table inet fw; nft delete table inet sync; "
                     "nft delete table ip6 flowsync", check=False)
        self.node.sh("ip link del dum0 2>/dev/null", check=False)
        self.setup()
        self.opts = []

    def close(self):
        self.stop()
        self.detach()
