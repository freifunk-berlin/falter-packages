"""The bypass: accepted TCP and UDP packets forwarded from the uplink's tc
hook, past netfilter; and everything that must still take the normal path."""
import subprocess

from ..ns import kill
from ..scenario import scenario
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
        env.ok("run with FSTEST_BYPASS: nothing to see")
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


@scenario(gateways=2, tags={"tcp"})
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
