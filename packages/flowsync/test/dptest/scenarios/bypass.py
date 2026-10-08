"""The bypass: accepted TCP and UDP packets forwarded from the uplink's tc
hook, past netfilter; and everything that must still take the normal path."""
import subprocess

from ..gateway import MSS
from ..ns import kill
from ..scenario import scenario
from ..tunnel import GRE_MTU, WG_MTU, mesh_tunnel, route_via
from .common import out, read, rejects, replies, slow_server, synced
from .frag import echo_server, xchg


@scenario(gateways=2)
def bypass(env):
    """With --bypass the replies of known flows reach the client without
    passing the firewall's forward chain (its counters stay at zero), with
    the hop limit lowered as a router does. Unknown packets are rejected as
    before. The switch works at run time, without the daemon."""
    g0, g1 = env.g[:2]
    env.start("--bypass")
    fs = env.flow("udp", fw=g0, rev=g0)
    fa = env.flow("udp", fw=g0, rev=g1)
    fs.send()
    fa.send()
    synced(env, fa)
    env.check("symmetric: replies arrive, hop limit 63 (of 3)", replies(env, fs), 3)
    env.check("asymmetric: replies arrive (of 3)", replies(env, fa), 3)
    env.check("the firewall saw none of them (mark, established, rejected)",
              [g.fwc(c) for g in (g0, g1) for c in ("mark", "est", "rej")], [0] * 6)
    u = env.flow("udp", fw=g0, rev=g1)
    env.check("no flow: nothing passes (of 3)", replies(env, u), 0)
    env.check("g1 rejected them", g1.fwc("rej"), 3)
    g0.tick()
    g1.tick()
    env.check("the programs counted the bypassed packets", [g0.st("dp_in_bypass"),
                                                             g1.st("dp_in_bypass")], [3, 3])
    env.check("status says so", [g0.st("bypass"), g1.st("bypass")], [1, 1])

    env.check("switched off at run time", g1.cmd("bypass", "off"), "bypass off")
    env.check("replies still arrive (of 3)", replies(env, fa), 3)
    env.check("now through the firewall, on the mark", g1.fwc("mark"), 3)
    env.check("and on again", g1.cmd("bypass", "on"), "bypass on")
    env.check("replies arrive (of 3)", replies(env, fa), 3)
    env.check("past the firewall again", g1.fwc("mark"), 3)


@scenario(gateways=2)
def bypass_default_off(env):
    """Without the option nothing is bypassed."""
    if env.bypass:
        env.ok("run with DPTEST_BYPASS: nothing to see")
        return
    g0 = env.g[0]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()
    env.check("replies arrive (of 3)", replies(env, f), 3)
    g0.tick()
    env.check("none bypassed, bypass off", [g0.st("dp_in_bypass"), g0.st("bypass")], [0, 0])


@scenario(gateways=2)
def bypass_normal_path(env):
    """What the bypass must leave alone still works with it on: fragments
    (reassembled before the firewall), packets with extension headers, and a
    packet too big for the next link, which gets its ICMPv6 error from the
    normal path so that the server learns the path MTU."""
    g0, g1 = env.g[:2]
    env.start("--bypass")
    f = env.flow("udp", fw=g0, rev=g1, real=True)
    echo_server(env, f, 12)
    env.check("1 byte, echoed (after a retry at most)", xchg(env, f, 1) + xchg(env, f, 1), "RECV 1")
    synced(env, f)
    r0 = rejects(env)          # the first echo may have raced the announcement
    m0 = g1.fwc("mark")
    env.check("3000 bytes in fragments, echoed through g1", xchg(env, f, 3000), "^RECV 3000$")
    env.check("the fragments went through the firewall", g1.fwc("mark") - m0, lambda n: n >= 1)

    g1.node.sh("ip link set mesh0 mtu 1400")
    a = xchg(env, f, 1420)
    b = xchg(env, f, 1420)
    env.check("too big for g1's mesh link: lost once, then the server fragments [%s, %s]" % (a, b),
              b, "^RECV 1420$")
    g1.node.sh("ip link set mesh0 mtu 1500")

    e = env.flow("udp", fw=g0, rev=g1)
    e.send()
    synced(env, e)
    m0 = g1.fwc("mark")
    env.probe(env.sv, "v6ext", e.s, e.sport, e.c, e.cport, "dstopt")
    env.sleep(0.3)
    env.check("a packet with an extension header arrives", e.delivered("rev"), 1)
    env.check("through the firewall", g1.fwc("mark") - m0, 1)
    env.check("nothing rejected", rejects(env) - r0, 0)


def clamped(env, name, *opts):
    g0, g1 = env.g[:2]
    undo = slow_server(env)
    env.start(*opts)
    f = env.flow("tcp", fw=g0, rev=g1, real=True)
    srv = env.spawn(env.sv, "tcpsrv", f.s, f.sport, 1000, out=out(env, "srv"))
    env.sleep(0.3)
    env.spawn(env.cl, "tcpmss", f.c, f.cport, f.s, f.sport, 1000, 10, out=out(env, "cli")).wait()
    try:
        srv.wait(5)
    except subprocess.TimeoutExpired:
        kill(srv)
    res = read(out(env, "cli"))
    # what the socket reports is less the 12 bytes of the timestamp option
    env.check("%s: connected, and the client's MSS is the clamp's (unclamped it reads 1428)"
              % name, res, "^OK %d$" % (MSS - 12))
    env.check("nothing rejected", rejects(env), 0)
    undo()


@scenario(gateways=2)
def mss_clamp(env):
    """The firewall's MSS clamp reaches the SYN/ACK that comes in from the
    uplink for a known flow: it is accepted on its mark only behind the
    clamp rule."""
    clamped(env, "tables")


@scenario(gateways=2)
def mss_clamp_bypass(env):
    """The same with the bypass on: segments with SYN are never bypassed."""
    clamped(env, "bypass", "--bypass")


@scenario(gateways=2)
def bypass_tcp(env):
    """Real TCP connections over the asymmetric path with the bypass: the
    SYN/ACK takes the normal path (where the MSS is clamped), the data does
    not. 4 connections of 200 kB each way complete."""
    g0, g1 = env.g[:2]
    undo = slow_server(env)
    env.start("--bypass")
    res = []
    for k in range(4):
        f = env.flow("tcp", fw=g0, rev=g1, real=True)
        srv = env.spawn(env.sv, "tcpsrv", f.s, f.sport, 200000, out=out(env, "srv%d" % k))
        env.sleep(0.3)
        cli = env.spawn(env.cl, "tcpcli", f.c, f.cport, f.s, f.sport, 200000, 10,
                        out=out(env, "cli%d" % k))
        cli.wait()
        try:
            srv.wait(5)
        except subprocess.TimeoutExpired:
            kill(srv)
        res.append(read(out(env, "cli%d" % k)))
    env.ok("connections [OK connect_ms total_ms]: %s" % "; ".join(res))
    env.check("connections completed (of 4)", len([r for r in res if r.startswith("OK")]), 4)
    env.check("nothing rejected", rejects(env), 0)
    env.check("SYN/ACKs and FINs went through the firewall, one or two per connection",
              g1.fwc("mark"), lambda n: 4 <= n <= 12)
    g1.tick()
    env.check("the data was bypassed (packets)", g1.st("dp_in_bypass"), lambda n: n >= 40)
    undo()


def otherhost(env, f, n=3):
    """n replies of f in frames addressed to another host's MAC, from the
    server side into g0's uplink; how many reached the client"""
    d0 = f.delivered("rev")
    for _ in range(n):
        env.probe(env.sv, "l2udp", "w0", f.s, f.sport, f.c, f.cport)
    env.sleep(0.3)
    return f.delivered("rev") - d0


@scenario(gateways=2)
def bypass_otherhost(env):
    """A frame addressed to another host's MAC, which a NIC in promiscuous
    mode (tcpdump, a bridged trunk) receives, is dropped by the stack. The tc
    program sees it first and must not forward it either, flow or not."""
    g0 = env.g[0]
    env.start("--bypass")
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()
    env.check("replies pass (of 3)", replies(env, f), 3)
    env.check("bypass on: frames for another MAC are not forwarded (of 3)", otherhost(env, f), 0)
    env.check("switched off", g0.cmd("bypass", "off"), "bypass off")
    env.check("bypass off: not forwarded either (of 3)", otherhost(env, f), 0)
    env.check("replies still pass (of 3)", replies(env, f), 3)
    env.check("nothing rejected", rejects(env), 0)


@scenario(gateways=2)
def bypass_fwmark(env):
    """Policy routing by packet mark holds for bypassed packets as for those
    on the normal path, which are routed with the mark the ingress program
    gave them: the bypass looks its route up with it."""
    g0 = env.g[0]
    env.start("--bypass")
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()
    env.check("replies arrive (of 3)", replies(env, f), 3)
    g0.node.sh("ip -6 route add blackhole default table 77 && "
               "ip -6 rule add fwmark 0x01000000/0x01000000 lookup 77")
    try:
        env.check("a rule routes marked packets into a blackhole: none arrives (of 3)",
                  replies(env, f), 0)
        g0.tick()
        env.check("none was bypassed past the rule", g0.st("dp_in_bypass"), 3)
    finally:
        g0.node.sh("ip -6 rule del fwmark 0x01000000/0x01000000 lookup 77; "
                   "ip -6 route flush table 77", check=False)
    env.check("without the rule they arrive again (of 3)", replies(env, f), 3)


def bypass_tunnel(env, kind):
    """the mesh side of g0 and g1 is a tunnel of the given kind; first without
    the bypass (the plumbing), then with it"""
    g0, g1 = env.g[:2]
    undo_srv = slow_server(env)
    env.start()
    undo = [mesh_tunnel(env, g, kind) for g in (g0, g1)]
    try:
        fs = env.flow("udp", fw=g0, rev=g0)
        fa = env.flow("udp", fw=g0, rev=g1)
        route_via(env, fs)
        route_via(env, fa)
        fs.send()
        fa.send()
        synced(env, fa)
        env.check("bypass off: replies through g0's tunnel (of 3)", replies(env, fs), 3)
        env.check("bypass off: replies through g1's tunnel (of 3)", replies(env, fa), 3)
        env.check("nothing rejected", rejects(env), 0)

        for g in (g0, g1):
            env.check("%s: bypass on" % g, g.cmd("bypass", "on"), "bypass on")
        m0 = [g.fwc("mark") for g in (g0, g1)]
        env.check("bypass on: replies through g0's tunnel (of 3)", replies(env, fs), 3)
        env.check("bypass on: replies through g1's tunnel (of 3)", replies(env, fa), 3)
        env.check("the firewall saw none of them",
                  [g.fwc("mark") - m for g, m in zip((g0, g1), m0)], [0, 0])
        for g in (g0, g1):
            g.tick()
        env.check("the programs forwarded them into the tunnels",
                  [g.st("dp_in_bypass") for g in (g0, g1)], lambda n: n[0] >= 3 and n[1] >= 3)
        env.check("nothing rejected", rejects(env), 0)

        # too big for the tunnel: the stack's path MTU error, not the bypass
        mtu = GRE_MTU if kind == "gre" else WG_MTU
        f = env.flow("udp", fw=g0, rev=g1, real=True)
        route_via(env, f)
        echo_server(env, f, 12)
        env.check("1 byte, echoed (after a retry at most)", xchg(env, f, 1) + xchg(env, f, 1), "RECV 1")
        synced(env, f)
        g1.tick()
        b0 = g1.st("dp_in_bypass")
        a = xchg(env, f, mtu + 20)
        b = xchg(env, f, mtu + 20)
        env.check("%d bytes, too big for the tunnel: lost once, then fragmented [%s, %s]"
                  % (mtu + 20, a, b), b, "^RECV %d$" % (mtu + 20))
        g1.tick()
        env.check("those took the normal path (none bypassed)", g1.st("dp_in_bypass") - b0, 0)

        # a real connection through the tunnel
        t = env.flow("tcp", fw=g0, rev=g1, real=True)
        route_via(env, t)
        b0 = g1.st("dp_in_bypass")
        srv = env.spawn(env.sv, "tcpsrv", t.s, t.sport, 200000, out=out(env, "srv"))
        env.sleep(0.3)
        env.spawn(env.cl, "tcpcli", t.c, t.cport, t.s, t.sport, 200000, 10,
                  out=out(env, "cli")).wait()
        try:
            srv.wait(5)
        except subprocess.TimeoutExpired:
            kill(srv)
        res = read(out(env, "cli"))
        env.check("TCP, 200 kB each way through the tunnel [%s]" % res, res, "^OK")
        g1.tick()
        env.check("the data was bypassed into the tunnel (packets)", g1.st("dp_in_bypass") - b0,
                  lambda n: n >= 40)
        env.check("nothing rejected", rejects(env), 0)
    finally:
        for u in undo:
            u()
        undo_srv()


@scenario(gateways=2)
def bypass_gre(env):
    """The bypass into an IPv4 GRE tunnel (the gateways' gre4-* devices, MTU
    1476): replies and TCP data are forwarded from tc into the tunnel, what
    does not fit takes the normal path and gets the stack's error."""
    bypass_tunnel(env, "gre")


@scenario(gateways=2)
def bypass_wg(env):
    """The same into a WireGuard tunnel (the corerouters' wg_* devices, MTU
    1280)."""
    bypass_tunnel(env, "wg")
