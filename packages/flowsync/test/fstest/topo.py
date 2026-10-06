"""The routed topology of one scenario run.

  hub: br0 10.0.0.250/24, the sync bridge; port s<i> per gateway
  g<i>: eth0 10.0.0.<i+1> (sync), mesh0 fd00:<i>:1::1 -> CL, wan0 fd00:<i>:2::1 -> SV
  CL: m<i> fd00:<i>:1::c, client addresses on lo; SV: w<i> fd00:<i>:2::5, servers on lo

Every flow has its own client and server address; CL routes the server via
the flow's forward gateway, SV routes the client via its reverse gateway (host
routes). Packets with SO_MARK i+1 take gateway i whatever the host route says
(policy routing on CL and SV). CL and SV count each flow's packets that came
through a gateway (hop limit 63) and drop them unless the flow uses real
sockets, so raw packets never draw a RST or ICMP from the endpoints.
"""
from .gateway import PREFIX, SERVER_NET, Gateway
from .ns import Node, kill


class Topology:
    def __init__(self, env, combo):
        self.env = env
        self.combo = combo
        self.hub = Node("hub")
        self.nodes = []
        self.flows = []         # flows of the current scenario, undone by reset()
        self.next_flow = 0      # flow ids keep counting across scenarios
        try:
            self.cl = self._node("CL")
            self.sv = self._node("SV")
            self.g = [Gateway(env, i, self._node("g%d" % i), p) for i, p in enumerate(combo.p)]
            self._build()
        except Exception:
            self.close()
            raise

    def _node(self, name):
        n = Node.create(name)
        self.nodes.append(n)
        return n

    def _build(self):
        hub, cl, sv = self.hub, self.cl, self.sv
        # no duplicate address detection anywhere: links come up usable at once
        # (a tentative link-local address leaves neighbours INCOMPLETE and the
        # first packets of a scenario lost); set before any link exists, since
        # a new device copies the "default" settings of its namespace
        for n in [hub] + self.nodes:
            n.sysctl(**{"net.ipv6.conf.default.accept_dad": 0, "net.ipv6.conf.all.accept_dad": 0,
                        "net.ipv6.conf.default.dad_transmits": 0})
        hub.sh("ip link del br0 2>/dev/null; ip link set lo up; ip link add br0 type bridge && "
               "ip link set br0 up && ip addr add 10.0.0.250/24 dev br0")
        for n in (cl, sv):
            n.sh("ip link set lo up")
        for g in self.g:
            i, gn = g.i, g.node
            hub.sh("ip link del s%d 2>/dev/null; ip link add s%d type veth peer name eth0 netns %d && "
                   "ip link set s%d master br0 up" % (i, i, gn.pid, i))
            gn.sh("ip link set lo up && ip link set eth0 up && ip addr add %s/24 dev eth0 && "
                  "ip link add mesh0 type veth peer name m%d netns %d && "
                  "ip link add wan0 type veth peer name w%d netns %d && "
                  "ip link set mesh0 up && ip link set wan0 up && "
                  "ip -6 addr add fd00:%d:1::1/64 dev mesh0 nodad && "
                  "ip -6 addr add fd00:%d:2::1/64 dev wan0 nodad && "
                  "ip -6 route add %s via fd00:%d:1::c && "
                  "ip -6 route add %s via fd00:%d:2::5"
                  % (g.addr, i, cl.pid, i, sv.pid, i, i, PREFIX, i, SERVER_NET, i))
            cl.sh("ip link set m%d up && ip -6 addr add fd00:%d:1::c/64 dev m%d nodad && "
                  "ip -6 rule add fwmark %d table %d && "
                  "ip -6 route add default via fd00:%d:1::1 dev m%d table %d"
                  % (i, i, i, i + 1, 100 + i, i, i, 100 + i))
            sv.sh("ip link set w%d up && ip -6 addr add fd00:%d:2::5/64 dev w%d nodad && "
                  "ip -6 rule add fwmark %d table %d && "
                  "ip -6 route add default via fd00:%d:2::1 dev w%d table %d"
                  % (i, i, i, i + 1, 100 + i, i, i, 100 + i))
            g.setup()
        for n in (cl, sv):
            n.nft("table inet ep {\n\tchain in {\n\t\ttype filter hook input priority 0;\n\t}\n}\n")

    def bind(self, env):
        """reuse this topology for another scenario run"""
        self.env = env
        for g in self.g:
            g.bind(env)

    def reset(self):
        """back to the state after the build: daemons stopped, tables flushed,
        flows (addresses, routes, counters) and servers gone, timers restored"""
        for f in self.flows:
            f.close()
        self.flows = []
        for g in self.g:
            g.reset()
        for n in (self.cl, self.sv):
            for p in n.spawned:
                kill(p)
            n.spawned = []
            n.sh("nft delete table inet ep 2>/dev/null", check=False)
            n.nft("table inet ep {\n\tchain in {\n\t\ttype filter hook input priority 0;\n\t}\n}\n")
        for g in self.g:
            for p in g.node.spawned:
                kill(p)
            g.node.spawned = []
        self.hub.sh("ip addr del 10.0.0.9/24 dev br0 2>/dev/null", check=False)

    def close(self):
        for g in getattr(self, "g", []):
            g.close()
        # the hub ends of the sync links go explicitly: a namespace is torn
        # down asynchronously by the kernel, and its veth peers in the hub can
        # outlive it into the next scenario's build
        for g in getattr(self, "g", []):
            self.hub.sh("ip link del s%d 2>/dev/null" % g.i, check=False)
        for n in self.nodes:
            n.close()
        self.hub.close()
        self.hub.sh("ip link del br0 2>/dev/null; ip addr del 10.0.0.9/24 dev br0 2>/dev/null",
                    check=False)
