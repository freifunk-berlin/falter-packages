"""Flows: a 5-tuple with a forward and a reverse path through the gateways.

fwd: client -> server (CL sends, SV receives); rev: server -> client.
"""
import re


class Flow:
    def __init__(self, env, proto="udp", fw=None, rev=None, real=False, cport=None, sport=None,
                 sink=True, client=None):
        self.env = env
        self.id = env.next_id()
        self.proto = proto
        # a /64 of its own inside the synced 2001:db8:100::/44, unless given
        self.c = client or "2001:db8:100:%x::1" % self.id
        self.s = "2a00:1450:4001:%x::e" % self.id
        self.cport = cport or 50000 + self.id
        self.sport = sport or (443 if proto == "udp" else 80)
        self.real = real
        env.topo.flows.append(self)
        self.fw = fw if fw is not None else env.g[0]
        self.rev = rev if rev is not None else (env.g[1] if len(env.g) > 1 else env.g[0])
        cl, sv = env.cl, env.sv
        cl.run("ip", "-6", "addr", "add", self.c + "/128", "dev", "lo", "nodad")
        sv.run("ip", "-6", "addr", "add", self.s + "/128", "dev", "lo", "nodad")
        self._routes()
        drop = sink and not real
        for node, src, dst, name in ((sv, self.c, self.s, "f%d" % self.id),
                                     (cl, self.s, self.c, "r%d" % self.id)):
            node.run("nft", "add", "counter", "inet", "ep", name)
            node.run("nft", "add", "rule", "inet", "ep", "in", "ip6", "saddr", src, "ip6", "daddr",
                     dst, "ip6", "hoplimit", "63", "counter", "name", name)
            if drop:
                node.run("nft", "add", "rule", "inet", "ep", "in", "ip6", "saddr", src, "ip6",
                         "daddr", dst, "drop")

    def close(self):
        """remove what the flow added to CL and SV (the counters and sink
        rules go with the ep table)"""
        cl, sv = self.env.cl, self.env.sv
        cl.run("ip", "-6", "route", "del", self.s + "/128", check=False)
        sv.run("ip", "-6", "route", "del", self.c + "/128", check=False)
        cl.run("ip", "-6", "addr", "del", self.c + "/128", "dev", "lo", check=False)
        sv.run("ip", "-6", "addr", "del", self.s + "/128", "dev", "lo", check=False)

    def _routes(self):
        self.env.cl.run("ip", "-6", "route", "replace", self.s + "/128", "via",
                        "fd00:%d:1::1" % self.fw.i, "dev", "m%d" % self.fw.i)
        self.env.sv.run("ip", "-6", "route", "replace", self.c + "/128", "via",
                        "fd00:%d:2::1" % self.rev.i, "dev", "w%d" % self.rev.i)

    def __repr__(self):
        return "%s [%s]:%d -> [%s]:%d" % (self.proto, self.c, self.cport, self.s, self.sport)

    def reroute(self, fw=None, rev=None):
        if fw is not None:
            self.fw = fw
        if rev is not None:
            self.rev = rev
        self._routes()

    # ------------------------------------------------------------ traffic
    def _probe(self, dir, via, *args, check=True):
        node = self.env.cl if dir == "fwd" else self.env.sv
        mark = ["--mark", via.i + 1] if via is not None else []
        return node.run(self.env.python, self.env.probe_path, *mark, *args, check=check)

    def _ends(self, dir):
        if dir == "fwd":
            return self.c, self.cport, self.s, self.sport
        return self.s, self.sport, self.c, self.cport

    def send(self, dir="fwd", via=None, n=1):
        """n UDP datagrams in one direction, via a gateway other than the route"""
        for _ in range(n):
            self._probe(dir, via, "send", *self._ends(dir))

    def tcp(self, dir, flags, seq, ack, plen=0, via=None, dport=None):
        src, sp, dst, dp = self._ends(dir)
        self._probe(dir, via, "tcp", src, sp, dst, dport or dp, flags, seq, ack, plen)

    def delivered(self, dir="fwd"):
        """packets of this flow that came through a gateway to the far end"""
        node, name = (self.env.sv, "f%d" % self.id) if dir == "fwd" else (self.env.cl,
                                                                        "r%d" % self.id)
        out = node.run("nft", "list", "counter", "inet", "ep", name, check=False)
        m = re.search(r"packets (\d+)", out)
        return int(m.group(1)) if m else 0


class Bulk(Flow):
    """many flows from one client to one server: client ports first..first+n-1"""

    def __init__(self, env, n, first=1024, sport=4000, **kw):
        super().__init__(env, "udp", sport=sport, **kw)
        self.n = n
        self.first = first

    def flood(self, via=None):
        """one raw datagram per flow, fast"""
        self._probe("fwd", via, "flood", self.c, self.s, self.sport, self.n, self.first)

    def burst(self, n=None, first=None, gap=0):
        """one datagram per flow through real sockets (slower)"""
        n = n or self.n
        first = first or self.first
        if gap:
            for k in range(n):
                self._probe("fwd", None, "send", self.c, first + k, self.s, self.sport)
                self.env.sleep(gap)
        else:
            self._probe("fwd", None, "burst", self.c, self.s, self.sport, n, first)
