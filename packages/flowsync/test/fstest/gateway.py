"""A gateway: profile-driven fw4-like ruleset, the flowsync daemon, conntrack
queries and controls on the sync path."""
import os
import re
import sys
import time

from .ns import kill

PORT = 3780
PREFIX = "2001:db8:100::/44"        # synced client prefix
XDST = "2001:db8::/32"              # the mesh: never synced as a server
SERVER_NET = "2a00:1450:4001::/48"  # where the test's servers live
MARK = 0x01000000


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
    """one conntrack entry as ctquery prints it, or none"""
    RE = re.compile(r"timeout=(\d+)s (SEEN_REPLY|UNREPLIED)( ASSURED)?( OFFLOAD)? mark=0x([0-9a-f]+)"
                    r"(?: tcp_state=(-?\d+))?")

    def __init__(self, text):
        self.raw = text.strip()
        m = self.RE.search(self.raw)
        self.alive = m is not None
        # the original tuple is the reverse of the one asked for
        self.reversed = self.alive and self.raw.endswith(" reversed")
        if not m and self.raw != "none":
            # a failed query is not an absent entry (wait_for retries it)
            raise RuntimeError("ctquery: %s" % (self.raw or "no output"))
        if not m:
            self.timeout, self.seen_reply, self.assured, self.offloaded = 0, False, False, False
            self.mark, self.tcp_state = 0, None
            return
        self.timeout = int(m.group(1))
        self.seen_reply = m.group(2) == "SEEN_REPLY"
        self.assured = bool(m.group(3))
        self.offloaded = bool(m.group(4))
        self.mark = int(m.group(5), 16)
        # -1: no protocol info in the dump (offloaded entries)
        self.tcp_state = int(m.group(6)) if m.group(6) not in (None, "-1") else None

    @property
    def is_copy(self):
        return self.alive and bool(self.mark & MARK)

    @property
    def is_native(self):
        return self.alive and not self.mark & MARK

    def carries_traffic(self, element_timeout):
        """saw a packet: ASSURED, and offloaded or above what a refresh sets"""
        return self.alive and self.assured and (self.offloaded or self.timeout > element_timeout)

    def __str__(self):
        return self.raw or "none"


class Gateway:
    def __init__(self, env, idx, node, profile):
        self.i = idx
        self.node = node
        self.p = profile
        self.addr = "10.0.0.%d" % (idx + 1)
        self.proc = None
        self.opts = []
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
        self.sysctls()
        self.node.nft(self.ruleset())

    def sysctls(self):
        """the scaled timers (scenarios may change them; reset() restores)"""
        t = self.env.timers
        self.node.sysctl(**{
            "net.ipv6.conf.all.forwarding": 1,
            "net.netfilter.nf_conntrack_udp_timeout": t["udp"],
            "net.netfilter.nf_conntrack_udp_timeout_stream": t["udp_stream"],
            "net.netfilter.nf_conntrack_tcp_timeout_syn_sent": t["tcp_syn_sent"],
            "net.netfilter.nf_conntrack_tcp_timeout_unacknowledged": 300,
            "net.netfilter.nf_conntrack_tcp_timeout_established": 432000,   # kernel default
            "net.netfilter.nf_conntrack_checksum": 0,
        })
        ft = "/proc/sys/net/netfilter/nf_flowtable_udp_timeout"
        if self.p.offload:
            for proto in ("udp", "tcp"):
                self.node.sh("f=/proc/sys/net/netfilter/nf_flowtable_%s_timeout; "
                             "[ -w $f ] && echo %d > $f || true" % (proto, t["flowtable"]))
        self.ft_idle = int(self.node.read(ft) or 30) if self.p.offload else 0

    def ruleset(self):
        off = self.p.offload
        ack = self.p.ack
        return """
table inet fw {
	counter fwd_inv {}
	counter fwd_est {}
	counter fwd_ack {}
	counter fwd_rej {}
%(ft)s
	chain forward {
		type filter hook forward priority 0; policy drop;
%(offload)s
		ct state invalid counter name fwd_inv
		ct state established,related counter name fwd_est accept
		iifname "mesh0" accept
%(ack)s
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
            "ft": "\tflowtable ft {\n\t\thook ingress priority 0; devices = { mesh0, wan0 };\n\t}"
                  if off else "",
            "offload": "\t\tmeta l4proto { tcp, udp } flow offload @ft" if off else "",
            "ack": ("\t\tmeta nfproto ipv6 tcp flags & ack == ack limit rate 5000/second "
                    "burst 2500 packets counter name fwd_ack accept\n"
                    "\t\tmeta nfproto ipv6 tcp flags & rst == rst limit rate 1000/second "
                    "burst 500 packets accept") if ack else "",
        }

    # ------------------------------------------------------------ daemon
    def start(self, *opts, debug=True):
        """start flowsync with the standard options plus opts (later options win);
        debug=False for scenarios that inject thousands of records a second"""
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
        argv = [sys.executable, env.ptyrun, env.flowsync, "-b", self.addr, "-I", "eth0"] + peers + [
            "-x", PREFIX, "-D", XDST, "-i", env.I, "-t", env.E, "-s", self.status
        ] + (["-d"] if debug else []) + self.opts + ["run"]
        log = open(self.logpath, "a")
        self.proc = self.node.spawn(*argv, stdout=log, stderr=log)
        log.close()

    def stop(self):
        """SIGTERM to flowsync itself (ptyrun's child, in a session of its own),
        so it shuts down cleanly; then the wrapper"""
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
        self.start(*(opts or self.opts), debug=getattr(self, "debug", True))

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

    # --------------------------------------------------------- conntrack
    def ct(self, flow, proto=None, sport=None):
        out = self.node.run(self.env.ctquery, "get", proto or flow.proto, flow.c, flow.cport,
                            flow.s, sport or flow.sport, check=False)
        return Entry(out)

    def ct_tuple(self, proto, c, cport, s, sport):
        """the entry for an arbitrary original tuple"""
        return Entry(self.node.run(self.env.ctquery, "get", proto, c, cport, s, sport, check=False))

    def set_sysctl(self, **kv):
        """net.netfilter.nf_conntrack_<key>=value for short keys, e.g. udp_timeout=300"""
        self.node.sysctl(**{"net.netfilter.nf_conntrack_" + k: v for k, v in kv.items()})

    def count(self, proto, which="all"):
        out = self.node.run(self.env.ctquery, "count", proto, which, check=False)
        m = re.search(r"count=(\d+)", out)
        return int(m.group(1)) if m else -1

    def flush(self):
        self.node.run(self.env.ctquery, "flush", check=False)

    def ct_count(self):
        return int(self.node.read("/proc/sys/net/netfilter/nf_conntrack_count") or 0)

    def fwc(self, name):
        """a forward chain counter: inv, est, ack, rej (not for offloaded packets)"""
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

    # ---------------------------------------------------- forwarded traffic
    def hold(self, flow):
        """drop the server's packets of a flow before conntrack sees them (TCP
        retransmits them), e.g. until the copy is in place"""
        self.node.nft("table inet hold {\n\tchain pre {\n\t\ttype filter hook prerouting "
                      "priority -400;\n\t\tip6 saddr %s ip6 daddr %s drop\n\t}\n}\n"
                      % (flow.s, flow.c))

    def release(self):
        self.node.sh("nft delete table inet hold 2>/dev/null", check=False)

    def reset(self):
        """back to the state after setup(), for the next scenario"""
        self.stop()
        self.unblock_sync()
        self.release()
        self.node.run("nft", "reset", "counters", "table", "inet", "fw", check=False)
        self.flush()
        self.sysctls()
        self.node.sh("ip link del dum0 2>/dev/null", check=False)
        self.opts = []

    def close(self):
        self.stop()
