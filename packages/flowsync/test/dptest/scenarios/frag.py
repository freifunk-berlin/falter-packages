"""Fragments and extension headers: what the tc programs have to parse
themselves, and the mark a reassembled packet takes from its first fragment."""
from ..scenario import scenario
from .common import rejects, replies, synced


def echo_server(env, f, count=8):
    env.spawn(env.sv, "echo", f.s, f.sport, 0, count)
    env.sleep(0.3)


def xchg(env, f, size, timeout=2):
    return env.probe(env.cl, "xchg", f.c, f.cport, f.s, f.sport, timeout, size)


@scenario(gateways=2)
def frag_sym(env):
    """Datagrams above the MTU, both ways through g0, real sockets: the
    flow's very first packet leaves in fragments (learned from the first
    one), and the fragmented echo comes back whole."""
    g0 = env.g[0]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0, real=True)
    echo_server(env, f)
    env.check("3000 bytes as the flow's first packet, echoed", xchg(env, f, 3000), "^RECV 3000$")
    env.check("20000 bytes (14 fragments), echoed", xchg(env, f, 20000), "^RECV 20000$")
    env.check("1 byte, echoed", xchg(env, f, 1), "^RECV 1$")
    env.check("nothing rejected", rejects(env), 0)
    env.check("conntrack accepted nothing", g0.fwc("est"), 0)
    env.check("one flow on g0", len(g0.flows("local")), 1)


@scenario(gateways=2)
def frag_asym(env):
    """The same out through g0 and back through g1: the fragmented echo is
    accepted on the flow g0 announced."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1, real=True)
    echo_server(env, f)
    env.check("1 byte, echoed (after a retry at most)",
              xchg(env, f, 1) + xchg(env, f, 1), "RECV 1")
    synced(env, f)
    r0 = rejects(env)
    env.check("3000 bytes, echoed through g1", xchg(env, f, 3000), "^RECV 3000$")
    env.check("20000 bytes, echoed through g1", xchg(env, f, 20000), "^RECV 20000$")
    env.check("nothing rejected", rejects(env) - r0, 0)
    env.check("g1 accepted the reassembled packets on the mark", g1.fwc("mark"),
              lambda n: n >= 2)


@scenario(gateways=2)
def frag_unsolicited(env):
    """Fragmented datagrams from outside without a flow, and to another port
    of a client that has one: none arrives."""
    g0 = env.g[0]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0, real=True)
    env.spawn(env.cl, "recv", f.c, f.cport, 3, out=env.dir + "/a.out")
    env.sleep(0.3)
    env.probe(env.sv, "send", f.s, f.sport, f.c, f.cport, 3000)
    env.sleep(3)
    env.check("no flow: the fragmented datagram does not arrive", open(env.dir + "/a.out").read(),
              "NONE")
    env.check("g0 rejected it", g0.fwc("rej"), lambda n: n >= 1)
    # now with a flow on the neighbouring port
    f.send()
    env.spawn(env.cl, "recv", f.c, f.cport + 1, 3, out=env.dir + "/b.out")
    env.spawn(env.cl, "recv", f.c, f.cport, 3, out=env.dir + "/c.out")
    env.sleep(0.3)
    env.probe(env.sv, "send", f.s, f.sport, f.c, f.cport + 1, 3000)
    env.probe(env.sv, "send", f.s, f.sport, f.c, f.cport, 3000)
    env.sleep(3)
    env.check("another client port: does not arrive either", open(env.dir + "/b.out").read(),
              "NONE")
    env.check("the flow's own port: the fragmented datagram arrives",
              open(env.dir + "/c.out").read(), "RECV")


@scenario(gateways=2)
def exthdr(env):
    """UDP behind extension headers (destination options, hop-by-hop, a
    fragment header on an unfragmented packet, all three in a row): the flow
    is found behind them, out and in."""
    g0, g1 = env.g[:2]
    env.start()
    for kind in ("dstopt", "hbh", "atomic", "chain"):
        f = env.flow("udp", fw=g0, rev=g1)
        env.probe(env.cl, "v6ext", f.c, f.cport, f.s, f.sport, kind)
        synced(env, f)
        env.check("%s out: g0 learned the flow" % kind, g0.ft(f), lambda e: e.local)
        env.check("%s out: plain replies pass g1 (of 2)" % kind, replies(env, f, 2), 2)
        d0 = f.delivered("rev")
        env.probe(env.sv, "v6ext", f.s, f.sport, f.c, f.cport, kind)
        env.sleep(0.3)
        env.check("%s in: passes g1" % kind, f.delivered("rev") - d0, 1)
        d0 = f.delivered("rev")
        env.probe(env.sv, "v6ext", f.s, f.sport, f.c, f.cport + 1, kind)
        env.sleep(0.3)
        env.check("%s in, to another port: rejected" % kind, f.delivered("rev") - d0, 0)
