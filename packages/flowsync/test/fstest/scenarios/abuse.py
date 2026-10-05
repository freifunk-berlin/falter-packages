"""Garbage, forged floods, event-filter noise, configuration defaults."""
import os

from ..gateway import PORT, PREFIX, XDST
from ..scenario import scenario


@scenario(gateways=2, once=True, tags={"abuse"})
def peer_spoof(env):
    """A host on the mesh side sends a resync request to g1's mesh address,
    with g0's IPv4 address as v4-mapped IPv6 source: to a socket on any
    address it looks exactly like g0's request. g1 listens on any address here
    (no bind_address). Without interface it must count as a non-peer's
    datagram; with interface eth0 (the sync link) it never reaches the daemon."""
    g0, g1 = env.g[:2]
    req = "01000100"            # version 1, no records, flags: resync
    g0.start()
    g1.start("-b", "", "-I", "")
    env.wait_for("daemons up", 8, lambda: g0.up() and g1.up())
    g1.block_sync(g0)           # g0's real requests and heartbeats stay out

    def forged():
        g1.tick()
        r0, b0 = g1.st("rx_resync") or 0, g1.st("rx_bad_peer") or 0
        env.probe(env.cl, "v6udp", "::ffff:" + g0.addr, 40000, "fd00:1:1::1", PORT, req)
        g1.tick()
        return (g1.st("rx_resync") or 0) - r0, (g1.st("rx_bad_peer") or 0) - b0

    env.check("no interface: not taken for g0's request, counted as a non-peer's",
              forged(), (0, 1))
    g1.restart("-b", "", "-I", "eth0")
    env.wait_for("g1 restarted with interface eth0", 8, g1.up)
    env.check("interface eth0: the datagram from the mesh side never reaches g1",
              forged(), (0, 0))


@scenario(gateways=2, once=True, tags={"abuse"})
def client_quota(env):
    """One client opens 500 flows (a scanner, P2P) while g1 may hold 400
    copies, 100 per client /64. The client gets its 100, and another client's
    new flow still gets its copy: one client cannot use up the copy pool of
    the fleet."""
    g0, g1 = env.g[:2]
    env.start("-C", 400, "-c", 100, debug=False)
    g0.set_sysctl(udp_timeout=300)
    b = env.bulk(500, fw=g0, rev=g1)
    b.flood()
    env.wait_for("g1 holds the client's 100 copies", 2 * env.I + 2,
                 lambda: g1.count("udp", "marked") == 100)
    env.wait_st("g1 refused the rest for that client", g1, "rx_limited_client", 1)
    f = env.flow("udp", fw=g0, rev=g1)
    f.send()
    env.wait_for("another client's flow gets its copy", 3, lambda: g1.ct(f).is_copy)
    env.check("the pool was never full", g1.st("rx_limited"), 0)


@scenario(gateways=2, tags={"abuse"})
def garbage(env):
    """Garbage from a peer, a datagram from a non-peer and a record outside the
    policy: all counted, nothing injected, the daemon keeps working."""
    g0, g1 = env.g[:2]
    env.start()
    env.probe(g1.node, "raw", "::ffff:" + g1.addr, 0, "::ffff:" + g0.addr, PORT, 200)
    env.hub.run("ip", "addr", "add", "10.0.0.9/24", "dev", "br0")
    env.probe(env.hub, "send", "::ffff:10.0.0.9", 0, "::ffff:" + g1.addr, PORT)
    env.hub.run("ip", "addr", "del", "10.0.0.9/24", "dev", "br0")
    g0.node.run(env.flowsync, "-e", g1.addr, "-x", PREFIX, "-D", XDST, "announce",
                "2001:db8:200::1", 50000, "2a00:1450:4001:ff::e", 443, check=False)
    env.sleep(env.I + 1)
    env.check("g0 counted the garbage", (g0.st("rx_parse") or 0) + (g0.st("rx_version") or 0),
              lambda n: 190 <= n <= 200)
    env.check("g0 injected nothing from it", g0.st("inject_created"), 0)
    env.wait_st("g1 counted the non-peer datagram", g1, "rx_bad_peer", 1)
    env.wait_st("g1 rejected the out-of-policy record", g1, "rx_policy", 1)
    env.check("g1 has no entry for it",
              g1.ct_tuple("udp", "2001:db8:200::1", 50000, "2a00:1450:4001:ff::e", 443).alive, False)
    # a flow to g1 itself: never asymmetric through another gateway, and a copy
    # would let that traffic in as established on any interface
    p0 = g1.st("rx_policy") or 0
    g0.node.run(env.flowsync, "-e", g1.addr, "-x", PREFIX, "-D", XDST, "announce",
                "2001:db8:100:ff::1", 50000, "fd00:1:2::1", 443, check=False)
    env.wait_st("g1 refused a copy of a flow to its own address", g1, "rx_policy", p0 + 1)
    env.check("g1 has no entry for it",
              g1.ct_tuple("udp", "2001:db8:100:ff::1", 50000, "fd00:1:2::1", 443).alive, False)
    r0 = g0.st("refresh_rounds") or 0
    f = env.flow("udp", fw=g0, rev=g1)
    f.send()
    env.wait_for("g0 still syncs a real flow", 3, lambda: g1.ct(f).is_copy)
    env.wait_for("g0's loop still runs rounds", 2 * env.I + 2,
                 lambda: (g0.st("refresh_rounds") or 0) > r0)


@scenario(gateways=2, once=True, tags={"abuse", "heavy"})
def spoof(env):
    """No authentication: a flood of policy-conforming records from a peer's
    address. The copy limit keeps the conntrack table from filling, and a
    legitimate copy survives. Refreshes come from g1's own dump, which walks
    every copy each round; with the flood that round takes seconds, so this
    scenario runs with element_timeout 6 x interval (production: 3 x 30 s
    against rounds of a few seconds) and without per-record debug logging."""
    g0, g1 = env.g[:2]
    env.start("-t", 6 * env.I, debug=False)
    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(16, 1.5, f.send)
    env.wait_for("g1 has the legitimate copy", 3, lambda: g1.ct(f).is_copy)
    out = os.path.join(env.dir, "spoof.out")
    p = env.spawn(g0.node, "spoof", "::ffff:" + g0.addr, "::ffff:" + g1.addr, PORT, 1000, 10,
                  out=out)
    env.hold("the legitimate copy survives the flood", 10, lambda: g1.ct(f).alive, step=0.5)
    p.wait()
    env.ok("%s; g1: conntrack=%d created=%s limited=%s errors=%s"
           % (open(out).read().strip(), g1.ct_count(), g1.st("inject_created"),
              g1.st("rx_limited"), g1.st("inject_errors")))
    env.check("no injection errors (the table never filled)", g1.st("inject_errors"), 0)
    env.check("the copy limit refused the rest", g1.st("rx_limited"), lambda n: n > 0)
    env.check("g1 holds no more copies than the limit (plus one receive pass)",
              g1.count("udp", "marked"), lambda n: n <= (g1.st("max_copies") or 0) + 2200)
    lp.wait()
    # the forged copies expire (nothing refreshes them): the pool frees up
    env.wait_for("the forged copies are gone", 6 * env.I + 4 * env.I,
                 lambda: g1.count("udp", "marked") <= 2)
    f2 = env.flow("udp", fw=g0, rev=g1)
    f2.send()
    env.wait_for("a new flow gets its copy again", 3, lambda: g1.ct(f2).is_copy)


@scenario(gateways=2, once=True, tags={"abuse"})
def noise(env):
    """What never reaches user space: IPv4 flows, protocols that are not
    configured, clients outside the prefixes; only the NEW event of a wanted
    flow is read. With more than 20 prefixes the prefix check moves to user
    space: foreign clients are read but not announced. (Gateway-local traffic:
    the point is the event filter, not the forwarding path.)"""
    g0, g1 = env.g[:2]
    env.start("-P", "udp")
    c, s = "2001:db8:100:ff::1", "2a00:1450:4001:ff::e"
    g0.node.sh("ip link add dum0 type dummy && ip link set dum0 up && "
               "ip addr add 192.0.2.1/24 dev dum0 && "
               "ip -6 addr add 2001:db8:200::1/128 dev dum0 nodad && "
               "ip -6 addr add %s/128 dev dum0 nodad && "
               "ip -6 route add 2a00:1450:4001:ff::/64 dev dum0" % c)

    def noise(first):
        env.probe(g0.node, "burst", "::ffff:192.0.2.1", "::ffff:192.0.2.2", 443, 300, first)
        env.probe(g0.node, "burst", "2001:db8:200::1", s, 443, 100, first)
        for i in range(1, 21):
            env.probe(g0.node, "tcp", c, first + i, s, 80, "S", 1000, 0)
        env.probe(g0.node, "icmp6", c, s, 20)

    ev0, tx0, c0 = g0.st("ev_recv") or 0, g0.st("tx_events") or 0, g0.ct_count()
    env.probe(g0.node, "send", c, 50000, s, 443)
    noise(40000)
    env.check("the kernel tracked the noise (new entries on g0)", g0.ct_count() - c0,
              lambda n: 400 <= n < 500)
    env.wait_for("the wanted flow syncs through the noise", 3,
                 lambda: g1.ct_tuple("udp", c, 50000, s, 443).alive)
    env.sleep(2 * env.I + 1)
    env.check("exactly one event reached user space", (g0.st("ev_recv") or 0) - ev0, 1)
    env.check("and only it was announced", (g0.st("tx_events") or 0) - tx0, 1)
    more = []
    for i in range(1, 25):
        more += ["-x", "2001:db8:%x::/48" % (0x1000 + i)]
    g0.restart("-P", "udp", *more)
    env.wait_for("g0 up with 25 prefixes", 8, g0.up)
    env.check("g0 logs the user-space fallback",
              sum(1 for l in g0.log().splitlines()
                  if "new events" in l and "more than 20 prefixes" in l), 1)
    ev0, tx0 = g0.st("ev_recv") or 0, g0.st("tx_events") or 0
    env.probe(g0.node, "send", c, 50001, s, 443)
    noise(41000)
    env.wait_for("the wanted flow syncs", 3, lambda: g1.ct_tuple("udp", c, 50001, s, 443).alive)
    env.sleep(2 * env.I + 1)
    env.check("the 100 foreign-prefix flows reach user space now, nothing else does",
              (g0.st("ev_recv") or 0) - ev0, 101)
    env.check("still only the wanted flow announced", (g0.st("tx_events") or 0) - tx0, 1)


@scenario(gateways=1, once=True, tags={"config"})
def default_tcp(env):
    """The default configuration syncs UDP and TCP."""
    out = env.hub.run(env.flowsync, "-x", PREFIX, "-e", "10.0.0.2", "check", check=False)
    env.check("the default configuration syncs TCP", out.count("\nproto tcp"), 1)
