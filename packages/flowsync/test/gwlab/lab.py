"""The lab: a topology description turned into namespaces, links with delay,
addresses and per-flow paths. It knows endpoints, links and gateways as boxes;
what runs on a gateway is the implementation's business (impl/).

  clients -- [mesh bridge] -- gateways -- [internet bridge] -- servers

  mesh:     fd00:1::/64   gateway i fd00:1::1:<i>   client k fd00:1::c:<k>
  internet: fd00:2::/64   gateway i fd00:2::1:<i>   server k fd00:2::5:<k>
  sync:     10.0.0.<i>/24 on the gateways' uplink (wan0), as in production
  flows:    client k uses 2001:db8:100:<k>::/64, server k 2a00:1450:4001:<k>::/64,
            one address each per flow

Every link is a veth with netem delay on both ends (netem delays egress
only); the bridges add nothing, so a path's delay is the sum of its links.
A flow's client routes the flow's server address via the forward gateway, its
server routes the client address via the return gateway.
"""
import fcntl
import glob
import os
import re

from .ns import Node

CLIENT_NET = "2001:db8:100::/44"        # what the gateways' clients use
MESH_NET = "2001:db8::/32"              # the mesh as a whole
SERVER_NET = "2a00:1450:4001::/48"


class Gateway:
    mesh_if, uplink_if = "mesh0", "wan0"
    client_net, mesh_net = CLIENT_NET, MESH_NET

    def __init__(self, lab, name, i, spec, policy):
        self.lab, self.name, self.i, self.timers = lab, name, i, lab.timers
        self.mesh_ms, self.uplink_ms = spec["mesh"], spec["uplink"]
        self.policy = {"bypass": False, "offload": False, **policy}
        self.node = Node.create(name)
        self.mesh6, self.uplink6, self.addr4 = "fd00:1::1:%x" % i, "fd00:2::1:%x" % i, "10.0.0.%d" % i
        self.peers4 = []
        self.dir = os.path.join(lab.dir, name)
        os.makedirs(self.dir, exist_ok=True)
        self.log = os.path.join(self.dir, "log")
        self.cgroup = lab.cgroup(name)
        self.napi = []

    def spawn(self, *argv):
        """a process of the implementation on this gateway: logged, and
        accounted to the gateway's CPU"""
        cg = self.cgroup

        def enter():
            if cg:
                with open(cg + "/cgroup.procs", "w") as f:
                    f.write(str(os.getpid()))
        return self.node.spawn(*argv, log=self.log, preexec=enter)

    def cpu_ms(self):
        """(user, system) CPU time of the implementation's processes, or None"""
        try:
            st = dict(line.split() for line in open(self.cgroup + "/cpu.stat"))
            return int(st["user_usec"]) / 1000, int(st["system_usec"]) / 1000
        except (OSError, TypeError, KeyError):
            return None

    def kernel_ms(self):
        """CPU time of the kernel's packet processing for this gateway: what
        arrives on mesh0 and wan0 is handled by threads of their own (threaded
        NAPI), through firewall, connection tracking, tc programs and
        forwarding. None unless the topology asks for kernel_cpu."""
        if not self.napi:
            return None
        ns = 0
        for pid in self.napi:
            try:
                ns += int(open("/proc/%s/schedstat" % pid).read().split()[0])
            except (OSError, ValueError):
                pass
        return ns / 1e6

    def sync_traffic(self):
        """(packets, bytes) the gateway sent on the sync path: IPv4 on the uplink"""
        out = self.node.run("nft", "list", "counter", "inet", "gwlab", "sync_tx", check=False)
        m = re.search(r"packets (\d+) bytes (\d+)", out)
        return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


class Endpoint:
    def __init__(self, name, k, role, ms):
        self.name, self.k, self.role, self.ms = name, k, role, ms
        self.node = Node.create(role[0] + name)
        self.lan6 = ("fd00:1::c:%x" if role == "client" else "fd00:2::5:%x") % k
        self.prefix = ("2001:db8:100:%x::" if role == "client" else "2a00:1450:4001:%x::") % k


class Lab:
    def __init__(self, topology, fleet, workdir, timers):
        self.dir, self.timers = workdir, timers
        os.makedirs(workdir, exist_ok=True)
        self._cg = self._cgroup_base()
        self.hub = Node("hub")
        self.gw, self.clients, self.servers = {}, {}, {}
        try:
            for i, (name, spec) in enumerate(topology["gateways"].items(), 1):
                self.gw[name] = Gateway(self, name, i, spec, fleet.get(name, {}))
            for k, (name, ms) in enumerate(topology["clients"].items(), 1):
                self.clients[name] = Endpoint(name, k, "client", ms)
            for k, (name, ms) in enumerate(topology["servers"].items(), 1):
                self.servers[name] = Endpoint(name, k, "server", ms)
            for g in self.gw.values():
                g.peers4 = [p.addr4 for p in self.gw.values() if p is not g]
            self._build(topology)
        except Exception:
            self.close()
            raise

    # ------------------------------------------------------------ cgroups
    def _cgroup_base(self):
        """the lab's own cgroup, if it was started in a delegated one
        (systemd-run --user --scope -p Delegate=yes): one child per gateway"""
        try:
            base = "/sys/fs/cgroup" + open("/proc/self/cgroup").read().split("::")[1].strip()
            os.makedirs(base + "/lab-%d" % os.getpid(), exist_ok=True)
            with open(base + "/lab-%d/cgroup.procs" % os.getpid(), "w") as f:
                f.write(str(os.getpid()))
            with open(base + "/cgroup.subtree_control", "w") as f:
                f.write("+cpu")
            return base
        except (OSError, IndexError):
            return None

    def cgroup(self, name):
        if not self._cg:
            return None
        path = "%s/%s-%d" % (self._cg, name, os.getpid())     # labs started by root share a parent
        try:
            os.makedirs(path, exist_ok=True)
            return path
        except OSError:
            return None

    # -------------------------------------------------------------- build
    def _build(self, topology):
        ends = list(self.clients.values()) + list(self.servers.values())
        nodad = {"net.ipv6.conf.default.accept_dad": 0, "net.ipv6.conf.all.accept_dad": 0,
                 "net.ipv6.conf.default.dad_transmits": 0}
        for n in [self.hub] + [g.node for g in self.gw.values()] + [e.node for e in ends]:
            n.sysctl(**nodad)
        hub = ["link add mesh type bridge", "link add inet type bridge",
               "link set mesh up", "link set inet up", "link set lo up"]
        delay = []      # (node, device, ms)
        for g in self.gw.values():
            for br, dev, ms in (("mesh", g.mesh_if, g.mesh_ms), ("inet", g.uplink_if, g.uplink_ms)):
                port = "%s-%s" % (br[0], g.name)
                hub += ["link add %s type veth peer name %s netns %d" % (port, dev, g.node.pid),
                        "link set %s master %s up" % (port, br)]
                delay += [(self.hub, port, ms), (g.node, dev, ms)]
        for e in ends:
            br = "mesh" if e.role == "client" else "inet"
            port = "%s-%s" % (br[0], e.node.name)
            hub += ["link add %s type veth peer name eth0 netns %d" % (port, e.node.pid),
                    "link set %s master %s up" % (port, br)]
            delay += [(self.hub, port, e.ms), (e.node, "eth0", e.ms)]
        self.hub.ip(hub)
        for g in self.gw.values():
            g.node.sysctl(**{"net.ipv6.conf.all.forwarding": 1})
            g.node.ip(["link set lo up", "link set mesh0 up", "link set wan0 up",
                       "addr add %s/64 dev mesh0 nodad" % g.mesh6,
                       "addr add %s/64 dev wan0 nodad" % g.uplink6,
                       "addr add %s/24 dev wan0" % g.addr4]
                      + ["route add %s/64 via %s" % (c.prefix, c.lan6) for c in self.clients.values()]
                      + ["route add %s/64 via %s" % (s.prefix, s.lan6) for s in self.servers.values()])
            # the lab's own observer: what the gateway sends on the sync path
            g.node.nft("table inet gwlab {\n\tcounter sync_tx {}\n\tchain out {\n"
                       "\t\ttype filter hook output priority -300;\n"
                       "\t\tmeta nfproto ipv4 oifname \"wan0\" counter name sync_tx\n\t}\n"
                       "\tchain cut_in {\n\t\ttype filter hook input priority -310;\n\t}\n"
                       "\tchain cut_out {\n\t\ttype filter hook output priority -310;\n\t}\n}\n")
        for e in ends:
            e.node.ip(["link set lo up", "link set eth0 up", "addr add %s/64 dev eth0 nodad" % e.lan6])
        for node, dev, ms in delay:
            if topology.get("offloads") is False:
                # packets as a router sees them, not 64 KiB aggregates
                node.run("ethtool", "-K", dev, "tso", "off", "gso", "off", "gro", "off", check=False)
            if ms:
                node.run("tc", "qdisc", "add", "dev", dev, "root", "netem", "delay", "%gms" % ms,
                         "limit", "100000")
        if topology.get("kernel_cpu"):
            self._threaded_napi()
        self._warm_up(ends)

    def _threaded_napi(self):
        """give every gateway's two interfaces a kernel thread for their
        receive path and remember the threads. veth has NAPI only with GRO on,
        and only for senders without TSO. The threads are named after the
        device, which every lab on the host has: they are found as the ones
        that appear when the switch is flipped, under a host-wide lock."""
        def threads():
            out = set()
            for p in glob.glob("/proc/[0-9]*/comm"):
                try:
                    if open(p).read().startswith("napi/"):
                        out.add(p.split("/")[2])
                except OSError:
                    pass
            return out
        with open("/tmp/gwlab-napi.lock", "a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            for g in self.gw.values():
                for br, dev in (("m", g.mesh_if), ("i", g.uplink_if)):
                    self.hub.run("ethtool", "-K", "%s-%s" % (br, g.name), "tso", "off", check=False)
                    g.node.run("ethtool", "-K", dev, "gro", "on", check=False)
                    before = threads()
                    # sysfs shows the devices of the namespace it was mounted in
                    g.node.run("unshare", "-m", "sh", "-c", "mount -t sysfs sysfs /sys && "
                               "echo 1 > /sys/class/net/%s/threaded" % dev, check=False)
                    g.napi += sorted(threads() - before)

    def _warm_up(self, ends):
        """resolve every neighbour now: a cold path costs the first packet of
        a flow three link delays, and the first sync datagram an ARP exchange"""
        for e in ends:
            gws = [g.mesh6 if e.role == "client" else g.uplink6 for g in self.gw.values()]
            e.node.sh(" ".join("ping -6 -c1 -W2 %s >/dev/null &" % a for a in gws) + " wait",
                      check=False)
        for g in self.gw.values():
            g.node.sh(" ".join("ping -c1 -W2 %s >/dev/null &" % a for a in g.peers4) + " wait",
                      check=False)

    # -------------------------------------------------------------- flows
    def place(self, flows):
        """give every flow its addresses, its path and its expected RTT"""
        ends = list(self.clients.values()) + list(self.servers.values())
        add = {e: [] for e in ends}
        first = getattr(self, "placed", 0) + 1
        self.placed = first + len(flows) - 1
        for n, f in enumerate(flows, first):
            c, s = self.clients[f["client"]], self.servers[f["server"]]
            fw, rv = self.gw[f["fwd"]], self.gw[f["rev"]]
            f["c"], f["s"] = "%s%x" % (c.prefix, n), "%s%x" % (s.prefix, n)
            f["asym"] = fw is not rv
            # the sync travels the uplinks like the answer does: what the answer
            # takes longer is the way to the server and back
            f["margin_ms"] = 2 * s.ms
            f["rtt_ms"] = (2 * (c.ms + s.ms) + fw.mesh_ms + fw.uplink_ms + rv.uplink_ms + rv.mesh_ms)
            add[c] += ["addr add %s/128 dev lo nodad" % f["c"],
                       "route add %s/128 via %s" % (f["s"], fw.mesh6)]
            add[s] += ["addr add %s/128 dev lo nodad" % f["s"],
                       "route add %s/128 via %s" % (f["c"], rv.uplink6)]
        for e in ends:
            if add[e]:
                e.node.ip(add[e])
        return flows

    # ------------------------------------------------------------- events
    # What a scenario can do to the lab while traffic runs. None of it knows
    # the implementation (losing state is the implementation's own verb).
    def reroute(self, flows, leg):
        """every flow's forward (leg "fwd") or return ("rev") path moves to
        the next gateway"""
        names = list(self.gw)
        cmds = {}
        for f in flows:
            new = self.gw[names[(names.index(f[leg]) + 1) % len(names)]]
            f[leg] = new.name
            if leg == "fwd":
                cmds.setdefault(self.clients[f["client"]], []).append(
                    "route replace %s/128 via %s" % (f["s"], new.mesh6))
            else:
                cmds.setdefault(self.servers[f["server"]], []).append(
                    "route replace %s/128 via %s" % (f["c"], new.uplink6))
        for e, lines in cmds.items():
            e.node.ip(lines)

    def sync_blackout(self, gw, on):
        """the gateway neither sends nor receives sync (IPv4 on its uplink)"""
        if on:
            gw.node.nft('add rule inet gwlab cut_in meta nfproto ipv4 iifname "wan0" drop\n'
                        'add rule inet gwlab cut_out meta nfproto ipv4 oifname "wan0" drop\n')
        else:
            gw.node.nft("flush chain inet gwlab cut_in\nflush chain inet gwlab cut_out\n")

    def uplink_recreate(self, gw):
        """the uplink device goes away and comes back under the same name and
        addresses with a new ifindex, as netifd does to a VLAN uplink"""
        port = "i-" + gw.name
        mac = gw.node.run("ip", "-o", "link", "show", "dev", "wan0").split("link/ether ")[1].split()[0]
        self.hub.ip(["link del %s" % port,       # a VLAN device comes back with its MAC address
                     "link add %s type veth peer name wan0 address %s netns %d" % (port, mac, gw.node.pid),
                     "link set %s master inet up" % port])
        gw.node.ip(["link set wan0 up", "addr add %s/64 dev wan0 nodad" % gw.uplink6,
                    "addr add %s/24 dev wan0" % gw.addr4]
                   + ["route add %s/64 via %s" % (s.prefix, s.lan6) for s in self.servers.values()])
        for node in (self.hub, gw.node):
            dev = port if node is self.hub else "wan0"
            if gw.uplink_ms:
                node.run("tc", "qdisc", "add", "dev", dev, "root", "netem", "delay",
                         "%gms" % gw.uplink_ms, "limit", "100000")

    def close(self):
        for g in getattr(self, "gw", {}).values():
            g.node.close()
        for e in list(getattr(self, "clients", {}).values()) + list(getattr(self, "servers", {}).values()):
            e.node.close()
        self.hub.sh("ip link del mesh 2>/dev/null; ip link del inet 2>/dev/null", check=False)
        for g in getattr(self, "gw", {}).values():
            try:
                os.rmdir(g.cgroup)
            except (OSError, TypeError):
                pass
