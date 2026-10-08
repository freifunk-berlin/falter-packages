"""A packet a client sends only because something reached it unasked (the
RST to a segment of no connection, the ICMPv6 error to a datagram for a
closed port, the ABORT to a stray SCTP packet) is an answer, not the start
of a flow. If the tables took it for one, whoever drew it would have made
their own way in: on the gateways the firewall lets some packets through
without a flow (TCP ACK and RST, rate-limited, and ICMPv6), and the client's
answer leaves through the uplink like any packet. One scenario per protocol
of the flow table."""
from ..scenario import scenario
from .common import synced

EST, CLOSING = 1, 2


def listed(g, which, f):
    """the lines of a table that name the flow's client"""
    return [ln for ln in g.flows(which) if f.c in ln]


def knocks(env, f, knock):
    """the server's packet through each of the two gateways; how many came
    through"""
    d0 = f.delivered("rev")
    for g in env.g[:2]:
        knock(g)
    env.sleep(0.3)
    return f.delivered("rev") - d0


def nothing_learned(env, f, what, knock):
    """after the client's answer left through g0: no flow there, none
    announced, and the server's packet passes neither gateway"""
    g0, g1 = env.g[:2]
    env.sleep(0.5)      # an announcement takes less
    env.check("%s left through g0" % what, f.delivered("fwd"), 1)
    env.check("%s: no flow on g0, none announced to g1" % what,
              [listed(g0, "local", f), listed(g1, "remote", f)], [[], []])
    env.check("%s: what the server sends passes neither gateway" % what,
              knocks(env, f, knock), 0)


def sv_probe(env, g, *args):
    env.probe(env.sv, "--mark", g.i + 1, *args)


@scenario(gateways=2)
def answer_tcp(env):
    """A RST for which there is no flow (the client's answer to an ACK or a
    SYN nobody asked for) makes none: the server's SYN on that tuple is
    rejected afterwards. A RST on a flow still closes it."""
    g0, g1 = env.g[:2]
    t = env.timers
    env.start()
    for flags, to in (("R", "an ACK"), ("RA", "a SYN")):
        f = env.flow("tcp", fw=g0, rev=g1)
        f.tcp("fwd", flags, 7000, 5001)
        nothing_learned(env, f, "the RST to %s" % to,
                        lambda g, f=f: f.tcp("rev", "S", 9000, 0, via=g))
    g0.tick()
    env.check("g0 counted the two", g0.st("dp_out_answer"), 2)
    f = env.flow("tcp", fw=g0, rev=g1)
    f.tcp("fwd", "A", 5000, 7000, 10)
    synced(env, f)
    env.check("a segment of a connection: a flow, listed under its client",
              [len(listed(g0, "local", f)), len(listed(g1, "remote", f))], [1, 1])
    f.tcp("fwd", "R", 5010, 0)
    env.check("a RST on that flow closes it", g0.ft(f),
              lambda e: e.flags == EST | CLOSING and 0 < e.local_left <= t["tcp_close"] + 1)
    env.check("the server's segments pass while it closes (of 2)",
              knocks(env, f, lambda g: f.tcp("rev", "A", 7000, 5010, via=g)), 2)


@scenario(gateways=2)
def answer_udp(env):
    """The ICMPv6 port unreachable a client sends for a datagram to a closed
    port quotes that datagram. It makes no UDP flow."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    env.probe(env.cl, "icmp6err", f.c, f.s, 1, 4, 17, f.sport, f.cport)
    nothing_learned(env, f, "the port unreachable", lambda g: f.send("rev", via=g))
    f.send()
    synced(env, f)
    env.check("a datagram of the client: a flow, listed under its client",
              [len(listed(g0, "local", f)), len(listed(g1, "remote", f))], [1, 1])
    env.check("now the server's datagrams pass (of 2)",
              knocks(env, f, lambda g: f.send("rev", via=g)), 2)


@scenario(gateways=2)
def answer_sctp(env):
    """SCTP, synced here: the ABORT a client sends for a packet of no
    association makes no flow. An INIT does."""
    g0, g1 = env.g[:2]
    env.start("-P", "sctp")
    f = env.flow("sctp", fw=g0, rev=g1)

    def init(g):
        sv_probe(env, g, "sctp", f.s, f.sport, f.c, f.cport, "init")

    env.probe(env.cl, "sctp", f.c, f.cport, f.s, f.sport, "abort")
    nothing_learned(env, f, "the ABORT", init)
    env.probe(env.cl, "icmp6err", f.c, f.s, 1, 4, 132, f.sport, f.cport)
    env.sleep(0.5)
    env.check("nor does a port unreachable quoting SCTP",
              [listed(g0, "local", f), listed(g1, "remote", f)], [[], []])
    g0.tick()
    env.check("g0 counted the ABORT", g0.st("dp_out_answer"), 1)
    env.probe(env.cl, "sctp", f.c, f.cport, f.s, f.sport, "init")
    synced(env, f)
    env.check("the client's INIT: a flow by its ports", g0.ft(f), lambda e: e.local)
    env.check("now the server's packets pass (of 2)", knocks(env, f, init), 2)


@scenario(gateways=2)
def answer_noports(env):
    """The protocols that are flows by their two addresses (ESP, GRE, IPv4
    and IPv6 in IPv6, L2TP, and one outside the list): a client without such
    a tunnel answers with an ICMPv6 parameter problem, unrecognized next
    header, or an address unreachable. Neither makes a flow of the quoted
    protocol."""
    g0, g1 = env.g[:2]
    env.start()
    for name, num, sync in (("esp", 50, True), ("gre", 47, True), ("ipip", 4, True),
                            ("ip6ip6", 41, True), ("l2tp", 115, True), ("99", 99, False)):
        f = env.flow("udp", fw=g0, rev=g1)

        def knock(g, f=f, num=num):
            sv_probe(env, g, "v6proto", f.s, f.c, num, 1)

        env.probe(env.cl, "icmp6err", f.c, f.s, 4, 1, num)
        nothing_learned(env, f, "%s, parameter problem" % name, knock)
        env.probe(env.cl, "icmp6err", f.c, f.s, 1, 3, num)
        env.sleep(0.5)
        env.check("%s, address unreachable: no flow either" % name,
                  [listed(g0, "local", f), listed(g1, "remote", f)], [[], []])
        env.probe(env.cl, "v6proto", f.c, f.s, num, 1)
        if sync:
            env.wait_for("%s: the client's packet is a flow, announced to g1" % name, 1,
                         lambda f=f, name=name: g1.ft_tuple(name, f.c, 0, f.s, 0).remote,
                         step=0.05)
        else:
            env.sleep(0.3)
        env.check("%s: now the server's packets pass (of 2)" % name, knocks(env, f, knock),
                  2 if sync else 1)


@scenario(gateways=2)
def answer_icmp6(env):
    """ICMPv6 has no flows: the echo reply a client sends to a ping from
    outside leaves no entry and opens the way for no other protocol between
    the two hosts."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)

    def knock(g):
        f.send("rev", via=g)
        f.tcp("rev", "S", 9000, 0, via=g)
        sv_probe(env, g, "v6proto", f.s, f.c, 99, 1)
        sv_probe(env, g, "v6proto", f.s, f.c, 47, 1)
        sv_probe(env, g, "sctp", f.s, f.sport, f.c, f.cport, "init")

    env.probe(env.cl, "icmp6", f.c, f.s, 1, 129)
    nothing_learned(env, f, "the echo reply", knock)
    env.check("no table holds anything", [g.flows(w) for g in (g0, g1)
                                          for w in ("local", "remote")], [[]] * 4)
