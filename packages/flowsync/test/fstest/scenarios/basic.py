"""What the flow tables accept and what they do not."""
import time

from ..scenario import scenario
from .common import rejects, replies, synced


@scenario(gateways=2)
def sym(env):
    """Both directions through g0: the reply is accepted on the local flow,
    without conntrack and without the daemon's help."""
    g0 = env.g[0]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()
    env.check("the packet reached the server", f.delivered("fwd"), 1)
    env.check("g0 has the flow, local", g0.ft(f), lambda e: e.local and not e.remote)
    env.check("replies pass g0 (of 3)", replies(env, f), 3)
    env.check("accepted on the mark (bypassed: the firewall saw none)", g0.fwc("mark"),
              0 if env.bypass else 3)
    env.check("conntrack accepted nothing", g0.fwc("est"), 0)
    env.check("nothing rejected", rejects(env), 0)


@scenario(gateways=2)
def unsolicited(env):
    """Packets from outside without a flow, to another port of a client with
    a flow, and from another port of its server: all rejected."""
    g0 = env.g[0]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0)
    env.check("no flow: nothing passes (of 3)", replies(env, f), 0)
    env.check("g0 rejected them", g0.fwc("rej"), 3)
    f.send()
    env.check("with the flow: replies pass (of 3)", replies(env, f), 3)
    d0 = f.delivered("rev")
    env.probe(env.sv, "send", f.s, f.sport + 1, f.c, f.cport)
    env.probe(env.sv, "send", f.s, f.sport, f.c, f.cport + 1)
    env.sleep(0.3)
    env.check("another server port and another client port: nothing passes",
              f.delivered("rev") - d0, 0)
    env.check("g0 rejected those too", g0.fwc("rej"), 5)


@scenario(gateways=2)
def asym(env):
    """Out through g0, back through g1: g1 accepts the reply on the flow g0
    announced. Every other gateway holds it too."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    f.send()
    synced(env, f)
    env.check("g1 has it as a peer's flow only", g1.ft(f), lambda e: e.remote and not e.local)
    env.check("replies pass g1 (of 3)", replies(env, f), 3)
    env.check("accepted on the mark (bypassed: the firewall saw none)", g1.fwc("mark"),
              0 if env.bypass else 3)
    env.check("nothing rejected", rejects(env), 0)
    env.wait_st("g0 announced it from the event", g0, "tx_events", 1)


@scenario(gateways=2)
def latency(env):
    """How long after a new flow's first packet through g0 the flow exists on
    g1 (20 flows on idle gateways): a reply faster than that is rejected."""
    g0, g1 = env.g[:2]
    env.start()
    lat = []
    for _ in range(20):
        f = env.flow("udp", fw=g0, rev=g1)
        t0 = time.monotonic()
        f.send()
        while not g1.ft(f).remote and time.monotonic() - t0 < 3:
            pass
        lat.append(time.monotonic() - t0)
        env.sleep(0.2)
    lat.sort()
    env.ok("ms until the flow is on g1 (includes one query, a few ms): min %d median %d max %d"
           % tuple(1000 * x for x in (lat[0], lat[10], lat[-1])))
    env.check("every flow within 100 ms (max ms)", int(1000 * lat[-1]), lambda n: n <= 100)


@scenario(gateways=2)
def expiry(env):
    """The client stops: its flow leaves g0 after the UDP timeout and g1 an
    element_timeout after g0's last announcement; then replies are rejected
    on both, and both tables are empty again."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    f.send()
    synced(env, f)
    t = env.timers["udp"]
    env.wait_for("g0's flow expires after the UDP timeout", t + 3, lambda: not g0.ft(f).local)
    env.wait_for("g1's copy expires an element_timeout later", env.E + env.I + 2,
                 lambda: not g1.ft(f).remote)
    env.check("no reply passes g1 (of 2)", replies(env, f, 2), 0)
    env.check("no reply passes g0 (of 2)", replies(env, f, 2, via=g0), 0)
    env.wait_for("the rounds removed the entries from the maps", 2 * env.I + 2,
                 lambda: not g0.flows("local") and not g1.flows("remote"))
    env.wait_st("g0 counted the expiry", g0, "local_expired", 1)
    env.wait_st("g1 counted the expiry", g1, "remote_expired", 1)


@scenario(gateways=2)
def server_cannot_hold(env):
    """Only the client's packets keep a flow alive: a server that keeps
    sending after the client fell silent is cut off at the UDP timeout."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    f.send()
    synced(env, f)
    lp = env.loop(200, 0.25, lambda: f.send("rev"))
    env.sleep(3)
    env.check("the server's packets pass while the flow lives", f.delivered("rev"), lambda n: n >= 8)
    env.wait_for("g0's flow expires all the same", env.timers["udp"] + 2,
                 lambda: not g0.ft(f).local)
    env.wait_for("and g1's copy", env.E + env.I + 2, lambda: not g1.ft(f).remote)
    d0 = f.delivered("rev")
    env.sleep(2)
    env.check("the server's packets no longer pass", f.delivered("rev") - d0, 0)
    lp.stop()


@scenario(gateways=3)
def long_flow(env):
    """A two-way UDP flow for 2.5 UDP timeouts and 3 element_timeouts: out
    through g0 every second, back through g1 five times a second; g2 sees
    none of it. Nothing may be rejected."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    f.send()
    synced(env, f)
    n = int(2.5 * env.timers["udp"])
    d0 = f.delivered("rev")
    for _ in range(n):
        f.send()
        for _ in range(5):
            f.send("rev")
            env.sleep(0.12)
    env.sleep(0.3)
    env.check("server -> client delivered (of %d)" % (5 * n), f.delivered("rev") - d0, 5 * n)
    env.check("nothing rejected", rejects(env), 0)
    env.check("g2 holds the flow as well", g2.ft(f), lambda e: e.remote)
    env.check("no gateway tracked anything forwarded", [g.fwc("est") for g in env.g], [0] * 3)


@scenario(gateways=3)
def reroute(env):
    """The forward path moves from g0 to g2 and the reply path from g1 to g0:
    whoever forwards the client's packets announces the flow, every gateway
    holds it, so replies pass wherever they come in, also after g0's own
    entry has expired."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    f.send()
    synced(env, f)
    env.check("replies pass g1 (of 3)", replies(env, f), 3)
    f.reroute(fw=g2)
    lp = env.loop(100, 1, f.send)
    env.wait_for("g2 has the flow as its own", 2, lambda: g2.ft(f).local)
    env.wait_for("g0's own entry expires", env.timers["udp"] + 3, lambda: not g0.ft(f).local)
    env.check("g0 and g1 hold it from g2", [g0.ft(f).remote, g1.ft(f).remote], [True, True])
    env.check("replies still pass g1 (of 3)", replies(env, f), 3)
    f.reroute(rev=g0)
    env.check("and g0, where they come in now (of 3)", replies(env, f), 3)
    env.check("nothing rejected", rejects(env), 0)
    lp.stop()


@scenario(gateways=2)
def policy(env):
    """Flows that are not synced (UDP to port 53, a server inside the mesh
    prefix) are still local flows: their replies pass the gateway that
    forwarded them and no other."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0, sport=53)
    f.send()
    env.check("replies pass g0 (of 3)", replies(env, f), 3)
    env.sleep(1)
    env.check("g1 does not hold it", g1.ft(f), lambda e: not e.alive)
    env.check("no reply passes g1 (of 2)", replies(env, f, 2, via=g1), 0)
    env.check("g0 announced nothing", g0.st("tx_events") or 0, 0)
    t = env.flow("tcp", fw=g0, rev=g1, sport=53)
    t.tcp("fwd", "S", 100, 0)
    synced(env, t)
    env.ok("TCP to port 53 is synced")


@scenario(gateways=2)
def tunnel_protos(env):
    """ESP, GRE, IPv4 in IPv6, IPv6 in IPv6 and L2TP are flows by their two
    addresses and synced by default: out through g0, back through g1."""
    g0, g1 = env.g[:2]
    env.start()
    for name, num in (("esp", 50), ("gre", 47), ("ipip", 4), ("ip6ip6", 41), ("l2tp", 115)):
        f = env.flow("udp", fw=g0, rev=g1)
        env.probe(env.sv, "v6proto", f.s, f.c, num, 2)
        env.sleep(0.3)
        env.check("%s unsolicited: nothing passes g1" % name, f.delivered("rev"), 0)
        env.probe(env.cl, "v6proto", f.c, f.s, num, 1)
        env.wait_for("%s: g1 holds the flow by its name" % name, 1,
                     lambda: g1.ft_tuple(name, f.c, 0, f.s, 0).remote, step=0.05)
        env.probe(env.sv, "v6proto", f.s, f.c, num, 3)
        env.sleep(0.3)
        env.check("%s: replies pass g1 (of 3)" % name, f.delivered("rev"), 3)
        env.check("%s: UDP between the same hosts is another flow, rejected" % name,
                  replies(env, f, 2), 0)


@scenario(gateways=2)
def proto_list(env):
    """The list is the administrator's: with `proto` given, only those are
    synced (ESP no longer), and a protocol can be named by its number."""
    g0, g1 = env.g[:2]
    env.start("-P", "udp", "-P", "tcp", "-P", "99")
    e = env.flow("udp", fw=g0, rev=g1)
    env.probe(env.cl, "v6proto", e.c, e.s, 50, 1)
    n = env.flow("udp", fw=g0, rev=g1)
    env.probe(env.cl, "v6proto", n.c, n.s, 99, 1)
    env.wait_for("protocol 99 is synced", 1, lambda: g1.ft_tuple(99, n.c, 0, n.s, 0).remote,
                 step=0.05)
    env.check("ESP is local to g0 now", [g0.ft_tuple("esp", e.c, 0, e.s, 0).local,
                                         g1.ft_tuple("esp", e.c, 0, e.s, 0).alive],
              [True, False])


@scenario(gateways=2)
def other_proto(env):
    """A protocol that is not in the list (99) is a local flow by its
    addresses: replies pass the gateway that forwarded it, are not synced,
    and nothing comes in unsolicited."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0)
    env.probe(env.sv, "v6proto", f.s, f.c, 99, 2)
    env.sleep(0.3)
    env.check("unsolicited: nothing passes", f.delivered("rev"), 0)
    env.probe(env.cl, "v6proto", f.c, f.s, 99, 1)
    env.sleep(0.3)
    env.check("the client's packet reached the server", f.delivered("fwd"), 1)
    env.check("g0 has it as a local flow without ports",
              [ln for ln in g0.flows("local") if " 99 " in ln and ":0 ->" in ln], lambda l: len(l) == 1)
    env.probe(env.sv, "v6proto", f.s, f.c, 99, 3)
    env.sleep(0.3)
    env.check("replies pass g0 (of 3)", f.delivered("rev"), 3)
    env.probe(env.sv, "--mark", g1.i + 1, "v6proto", f.s, f.c, 99, 2)
    env.sleep(0.3)
    env.check("none passes g1", f.delivered("rev"), 3)
    env.check("UDP between the same hosts is another flow: rejected", replies(env, f, 2), 0)


@scenario(gateways=3)
def idle_peers(env):
    """Nothing keeps itself alive: three gateways with one flow each, all
    stopped at once. Every table is empty after the UDP timeout plus an
    element_timeout, and no gateway announces anything any more."""
    env.start()
    fl = [env.flow("udp", fw=g, rev=env.g[(g.i + 1) % 3]) for g in env.g]
    for f in fl:
        f.send()
        synced(env, f)
    wait = env.timers["udp"] + env.E + 2 * env.I + 2
    env.wait_for("all tables empty", wait,
                 lambda: not any(g.flows("local") or g.flows("remote") for g in env.g), step=1)
    r0 = [g.st("tx_refresh") for g in env.g]
    for g in env.g:
        g.tick()
    env.sleep(env.I)
    env.check("no refreshes afterwards", [g.st("tx_refresh") for g in env.g], r0)
    env.check("no reply passes (of 2)", replies(env, fl[0], 2), 0)
