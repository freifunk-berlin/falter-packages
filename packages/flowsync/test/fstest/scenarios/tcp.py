"""TCP: split handshakes, pickup, RST, the SYN/ACK race, idle connections,
reversed entries, DNS over TCP, flushes without the stateless rule."""
import os

from ..scenario import need, scenario
from .common import CLOSE, EST, SYN_SENT, long_lived, native_unreplied, tcp_state

ACK_ON_G1 = need(lambda p: p[1].ack, "reversed entries need the stateless ACK rule on g1")
NO_ACK_ON_G1 = need(lambda p: not p[1].ack, "needs g1 without the stateless ACK rule")


@scenario(gateways=3, tags={"tcp"})
def tcp_handshake(env):
    """A TCP handshake split across two gateways: g0 sees the SYN and the
    client's packets, g1 the SYN/ACK and the server's data, which must pass on
    the injected ESTABLISHED copy."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1)
    f.tcp("fwd", "S", 1000, 0)
    env.wait_for("g0 has a SYN_SENT native", 2, lambda: g0.ct(f).alive)
    env.check("g0 native: SYN_SENT, unreplied, unmarked", g0.ct(f),
              lambda e: native_unreplied(e) and e.tcp_state == SYN_SENT)
    env.wait_for("g1 has the copy", 3, lambda: g1.ct(f).is_copy)
    env.check("g1 copy: ESTABLISHED, marked", g1.ct(f),
              lambda e: e.is_copy and e.seen_reply and e.tcp_state == EST)
    env.check("g2 copy: ESTABLISHED", g2.ct(f), lambda e: e.tcp_state == EST)
    d0 = f.delivered("rev")
    f.tcp("rev", "SA", 5000, 1001)
    f.tcp("rev", "PA", 5001, 1001, 10)
    env.wait_for("g1 passed the SYN/ACK and the server's data", 2,
                 lambda: f.delivered("rev") == d0 + 2)
    env.check("g1 copy ASSURED after traffic", g1.ct(f),
              lambda e: e.assured and tcp_state(e, EST))
    inv0 = g0.fwc("inv")
    f.tcp("fwd", "A", 1001, 5001)
    env.check("g0: the client's ACK is invalid in SYN_SENT", g0.fwc("inv") - inv0, 1)
    env.check("g0 native still SYN_SENT", g0.ct(f), lambda e: e.tcp_state == SYN_SENT)
    g1.block_sync()     # so that g1's dump sees the evidence before a refresh
    env.wait_st("g1 announced its copy from evidence", g1, "tx_copies", 1)
    env.wait_st("g0 counted g1's announcement as EEXIST", g0, "inject_exists", 1)
    env.check("g0 native left alone by it", g0.ct(f),
              lambda e: native_unreplied(e) and e.tcp_state == SYN_SENT)
    g1.unblock_sync()
    env.wait_for("g0's SYN_SENT expires", env.timers["tcp_syn_sent"] + 4,
                 lambda: not g0.ct(f).alive)
    f.tcp("rev", "PA", 5011, 1001, 10)      # the server keeps talking, only g1 sees it
    env.wait_for("g0 gets the flow back as a copy from g1", env.I + 3, lambda: g0.ct(f).alive)
    env.check("g0 copy marked, ESTABLISHED", g0.ct(f), lambda e: e.is_copy and e.tcp_state == EST)
    f.tcp("fwd", "PA", 1001, 5021, 10)
    env.check("g0 copy ASSURED from the client's data", g0.ct(f), lambda e: e.assured)
    env.wait_st("g0 announces its copy from that evidence", g0, "tx_copies", 1)
    env.check("g1 copy present throughout", g1.ct(f), lambda e: e.is_copy)
    ac = g0.st("tx_copies")
    env.sleep(2 * env.I + 1)
    env.check("g0's copy went silent after one round without packets", g0.st("tx_copies"), ac)
    g0.flush()
    env.wait_for("g2's copy (no traffic of its own) gone once nothing announces",
                 env.E + 3 * env.I, lambda: not g2.ct(f).alive)
    env.check("g1's copy lives on its own TCP timeout", g1.ct(f),
              lambda e: e.is_copy and e.assured)


@scenario(gateways=2, tags={"tcp"})
def tcp_pickup(env):
    """No handshake: data in the middle of a connection is picked up as
    ESTABLISHED (tcp_loose) and synced like any other flow."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1)
    f.tcp("fwd", "PA", 2000, 7000, 10)
    env.wait_for("g0 picks the flow up mid-stream", 2, lambda: g0.ct(f).alive)
    env.check("g0 native ESTABLISHED, unreplied, unmarked", g0.ct(f),
              lambda e: native_unreplied(e) and e.tcp_state == EST)
    env.wait_for("g1 has the copy", 3, lambda: g1.ct(f).is_copy)
    env.check("g1 copy ESTABLISHED", g1.ct(f), lambda e: e.tcp_state == EST)


@scenario(gateways=2, tags={"tcp"})
def tcp_rst(env):
    """The server resets. The copy goes to CLOSE, a refresh does not resurrect
    it, and it is gone once the origin stops announcing."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1)
    f.tcp("fwd", "PA", 2000, 7000, 10)
    env.wait_for("g1 has the copy", 4, lambda: g1.ct(f).is_copy)
    f.tcp("rev", "PA", 7000, 2010, 10)
    f.tcp("rev", "R", 7010, 2010)
    # an offloaded copy reaches CLOSE once the flowtable's gc has torn the
    # flow down (until then the dump shows no TCP state)
    env.wait_for("g1 copy in CLOSE after the RST", 1 + (g1.ft_idle and 3),
                 lambda: g1.ct(f).tcp_state == CLOSE, step=0.1)
    env.sleep(env.I + 2)
    env.check("a refresh does not resurrect the closed copy", g1.ct(f),
              lambda e: e.tcp_state == CLOSE)
    env.check("g0 native still ESTABLISHED (it never saw the RST)", g0.ct(f),
              lambda e: e.tcp_state == EST)
    g0.flush()
    env.wait_for("closed copy gone once nobody announces it", env.E + 3 * env.I,
                 lambda: not g1.ct(f).alive)


@scenario(gateways=2, tags={"tcp"})
def tcp_race(env):
    """The reply beats the announcement: a SYN/ACK without a copy is invalid
    (it passes only through a stateless ACK rule), the next refresh delivers
    the copy and the retransmission passes as established."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1)
    g1.block_sync()
    f.tcp("fwd", "S", 1000, 0)
    env.sleep(1)
    inv0, d0, est0 = g1.fwc("inv"), f.delivered("rev"), g1.fwc("est")
    f.tcp("rev", "SA", 5000, 1001)
    env.check("a SYN/ACK without a copy is invalid", g1.fwc("inv") - inv0, 1)
    env.sleep(0.3)
    env.check("it passes only through a stateless ACK rule",
              f.delivered("rev") - d0, 1 if g1.p.ack else 0)
    env.check("g1 has no entry", g1.ct(f).alive, False)
    g1.unblock_sync()
    env.wait_for("the next refresh delivers the copy", env.I + 2, lambda: g1.ct(f).is_copy)
    f.tcp("rev", "SA", 5000, 1001)
    env.sleep(0.3)
    env.check("the retransmitted SYN/ACK passes as established", g1.fwc("est") - est0, 1)


@scenario(gateways=2, tags={"tcp"})
def tcp_idle(env):
    """An idle asymmetric connection keeps its state on the reply gateway: the
    copy that saw the server's traffic is not cut back by refreshes, and the
    server may speak first after the forward gateway's entry expired.
    (Scaled: tcp_timeout_unacknowledged 15 s.)"""
    g0, g1 = env.g[:2]
    env.start()
    for g in env.g:
        g.set_sysctl(tcp_timeout_unacknowledged=15)
    f = env.flow("tcp", fw=g0, rev=g1)
    f.tcp("fwd", "PA", 2000, 7000, 10)
    env.wait_for("g1 has the copy", 4, lambda: g1.ct(f).is_copy)
    f.tcp("rev", "PA", 7000, 2010, 10)
    env.check("g1's copy got a long timeout from the server segment", g1.ct(f), long_lived)
    env.wait_for("g0's native expires (idle)", 20, lambda: not g0.ct(f).alive)
    env.sleep(env.E + 2 * env.I)
    env.check("g1 keeps the copy that saw traffic", g1.ct(f), lambda e: e.is_copy)
    d0 = f.delivered("rev")
    f.tcp("rev", "PA", 7010, 2010, 10)
    env.wait_for("the server speaks first after the idle period: delivered", 2,
                 lambda: f.delivered("rev") == d0 + 1)
    env.check("g1's entry is still the copy (no reversed pickup)", g1.ct(f), lambda e: e.is_copy)


@scenario(gateways=2, requires=ACK_ON_G1, tags={"tcp"})
def reversed(env):
    """A server segment that reaches g1 before the copy is picked up as a
    connection of its own, server -> client (the stateless ACK rule let it in).
    The next announcement replaces it with the copy."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1)
    g1.block_sync()
    f.tcp("fwd", "PA", 2000, 7000, 10)
    env.wait_for("g0 has the native", 2, lambda: g0.ct(f).alive)
    env.sleep(1)
    f.tcp("rev", "PA", 7000, 2010, 10)
    env.check("g1 picked the server segment up as a reversed entry", g1.ct(f),
              lambda e: native_unreplied(e) and e.tcp_state == EST)
    g1.unblock_sync()
    est0 = g1.fwc("est")
    for i in (1, 2, 3):
        f.tcp("fwd", "PA", 2010 + 10 * i, 7010, 10)
        f.tcp("rev", "PA", 7010 + 10 * i, 2020, 10)
        env.sleep(env.I)
    env.check("g1 holds flowsync's copy once sync works again", g1.ct(f), lambda e: e.is_copy)
    env.check("the server's segments pass g1 as established", g1.fwc("est") - est0,
              lambda n: n >= 1)
    env.check("g1 replaced the entry", g1.st("inject_replaced"), lambda n: n >= 1)


@scenario(gateways=2, requires=ACK_ON_G1, tags={"tcp"})
def reversed_real(env):
    """The same with real TCP stacks: the connection starts while announcements
    to g1 are blocked, g1 picks it up reversed; once sync works the copy
    replaces it and the server's segments are established."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1, real=True)
    g1.block_sync(g0)
    env.spawn(env.sv, "tcpecho", f.s, f.sport, 40)
    env.sleep(0.5)
    out = os.path.join(env.dir, "cli.out")
    cli = env.spawn(env.cl, "tcptalk", f.c, f.cport, f.s, f.sport, 40, 0.5, 10, out=out)
    env.sleep(3)
    env.check("g1 picked the connection up reversed", g1.ct(f), native_unreplied)
    g1.unblock_sync()
    env.sleep(2 * env.I + 1)
    est0 = g1.fwc("est")
    env.sleep(2)
    env.check("after sync resumes g1 holds the copy", g1.ct(f), lambda e: e.is_copy)
    env.true("the server's segments pass g1 as established",
             g1.fwc("est") > est0 or g1.ct(f).offloaded, g1.ct(f))
    cli.wait()
    env.check("the connection itself survives", open(out).read().strip(), "^OK 40$")


@scenario(gateways=2, tags={"tcp"})
def dns_tcp(env):
    """skip_server_port 53 applies to UDP only: DNS over TCP is synced."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1, sport=53)
    f.tcp("fwd", "PA", 2000, 7000, 10)
    env.wait_for("a TCP flow to port 53 is synced", 4, lambda: g1.ct(f).is_copy)


@scenario(gateways=2, requires=NO_ACK_ON_G1, tags={"tcp"})
def tcp_flush_norule(env):
    """Without the stateless ACK rule a flush on the reply gateway would reset
    live connections; the resync after the flush restores the copy before the
    server's next segment."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1, real=True)
    # generous timeouts: a lost state shows as a reset (no ACK rule), while a
    # flood elsewhere on the host (the per-CPU backlog is shared by all
    # namespaces) only delays segments into retransmissions
    env.spawn(env.sv, "tcpecho", f.s, f.sport, 60)
    env.sleep(0.5)
    out = os.path.join(env.dir, "cli.out")
    cli = env.spawn(env.cl, "tcptalk", f.c, f.cport, f.s, f.sport, 30, 1, 20, out=out)
    env.sleep(3)
    g0.tick()   # flush right after g0's round: without a resync the gap is an interval
    # and right after an exchange (the server only answers the client, so its
    # next segment comes a talk interval later): the resync has that long,
    # instead of racing a segment already in flight
    d0 = f.delivered("rev")
    env.wait_for("an exchange completed", 3, lambda: f.delivered("rev") > d0, step=0.02)
    g1.flush()
    cli.wait()
    env.check("the connection survives a flush on the reply gateway", open(out).read().strip(),
              "^OK 30$")
    env.ok("g1: copies_lost=%s tx_resync=%s; g0: rx_resync=%s"
           % (g1.st("copies_lost"), g1.st("tx_resync"), g0.st("rx_resync")))
