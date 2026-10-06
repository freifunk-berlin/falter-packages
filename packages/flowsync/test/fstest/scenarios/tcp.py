"""TCP: the little state the egress program keeps (handshake seen, closing),
and real connections over asymmetric paths."""
import subprocess

from ..ns import kill
from ..scenario import need, scenario
from .common import out, read, rejects, slow_server, synced

EST, CLOSING = 1, 2
NO_ACK = need(lambda p: not any(g.ack for g in p), "the stateless ACK rule would carry it")


@scenario(gateways=2, tags={"tcp"})
def tcp_state(env):
    """The lifetime of a TCP flow follows the client's segments: a SYN alone
    gets the SYN timeout, the next segment the established one, a FIN the
    closing one, and a new SYN on the tuple starts over."""
    g0, g1 = env.g[:2]
    t = env.timers
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1)
    f.tcp("fwd", "S", 100, 0)
    synced(env, f)
    env.check("after the SYN: not established, SYN timeout", g0.ft(f),
              lambda e: e.flags == 0 and t["tcp_syn"] - 2 <= e.local_left <= t["tcp_syn"] + 1)
    f.tcp("rev", "SA", 500, 101)
    env.sleep(0.3)
    env.check("the SYN/ACK passed g1", f.delivered("rev"), 1)
    env.check("and changed nothing on g0", g0.ft(f), lambda e: e.flags == 0)
    f.tcp("fwd", "A", 101, 501)
    env.check("after the ACK: established timeout", g0.ft(f),
              lambda e: e.flags == EST and e.local_left > t["tcp"] - 3)
    f.tcp("rev", "A", 501, 101, 100)
    env.sleep(0.3)
    env.check("server data passes g1", f.delivered("rev"), 2)
    f.tcp("fwd", "FA", 101, 601)
    env.check("after the client's FIN: closing timeout", g0.ft(f),
              lambda e: e.flags == EST | CLOSING and e.local_left <= t["tcp_close"] + 1)
    f.tcp("rev", "FA", 601, 102)
    env.sleep(0.3)
    env.check("the server's FIN still passes g1", f.delivered("rev"), 3)
    f.tcp("fwd", "S", 9000, 0)
    env.check("a new SYN on the tuple starts over", g0.ft(f), lambda e: e.flags == 0)
    env.check("nothing rejected", rejects(env), 0)


@scenario(gateways=2, tags={"tcp"})
def tcp_pickup(env):
    """No handshake seen (the gateway rebooted, or the path moved here): a
    segment in the middle of a connection makes an established flow."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1)
    f.tcp("fwd", "A", 5000, 7000, 10)
    synced(env, f)
    env.check("established at once", g0.ft(f),
              lambda e: e.flags == EST and e.local_left > env.timers["tcp"] - 3)
    f.tcp("rev", "A", 7000, 5010, 10)
    env.sleep(0.3)
    env.check("the server's segment passes g1", f.delivered("rev"), 1)


@scenario(gateways=2, tags={"tcp"})
def tcp_connect(env):
    """Real connections over the asymmetric path (SYN via g0, SYN/ACK via
    g1) to a server 20 ms away: each of 8 completes without a SYN/ACK
    retransmission, and nothing is rejected, whatever the profile."""
    g0, g1 = env.g[:2]
    undo = slow_server(env)
    env.start()
    res = []
    for k in range(8):
        f = env.flow("tcp", fw=g0, rev=g1, real=True)
        srv = env.spawn(env.sv, "tcpsrv", f.s, f.sport, 1000, out=out(env, "srv%d" % k))
        env.sleep(0.3)
        cli = env.spawn(env.cl, "tcpcli", f.c, f.cport, f.s, f.sport, 1000, 10,
                        out=out(env, "cli%d" % k))
        cli.wait()
        try:
            srv.wait(5)
        except subprocess.TimeoutExpired:
            kill(srv)
        res.append(read(out(env, "cli%d" % k)))
    env.ok("connections [OK connect_ms total_ms]: %s" % "; ".join(res))
    ok = [r for r in res if r.startswith("OK")]
    env.check("connections completed (of 8)", len(ok), 8)
    env.check("without a retransmission (of 8)", len([r for r in ok if int(r.split()[1]) < 500]),
              8)
    env.check("nothing rejected", rejects(env), 0)
    env.check("the stateless budget carried nothing", g1.fwc("ack"), 0)
    undo()


@scenario(gateways=2, tags={"tcp"})
def tcp_race(env):
    """The limit of any sync: a server next door answers faster than the
    announcement travels. With the stateless ACK rule the SYN/ACK passes on
    it; without, it is rejected and the client's SYN retransmission (1 s)
    finds the flow in place. Either way the connection completes."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1, real=True)
    srv = env.spawn(env.sv, "tcpsrv", f.s, f.sport, 1000, out=out(env, "srv"))
    env.sleep(0.3)
    cli = env.spawn(env.cl, "tcpcli", f.c, f.cport, f.s, f.sport, 1000, 10, out=out(env, "cli"))
    cli.wait()
    try:
        srv.wait(5)
    except subprocess.TimeoutExpired:
        kill(srv)
    res = read(out(env, "cli"))
    env.check("the connection completed", res, "^OK")
    env.ok("connect and total ms: %s; g1 accepted %d on the mark, %d on the budget, rejected %d"
           % (res, g1.fwc("mark"), g1.fwc("ack"), g1.fwc("rej")))


@scenario(gateways=2, requires=NO_ACK, tags={"tcp"})
def race_window(env):
    """How far away a server must be for its SYN/ACK not to beat the
    announcement: 4 connections each at 1, 2, 5 and 10 ms one way, without
    the stateless rule. Reported; at 5 ms and above none may need a
    retransmission."""
    g0, g1 = env.g[:2]
    env.start()
    k = 0
    for ms in (1, 2, 5, 10):
        undo = slow_server(env, ms)
        fast = 0
        for _ in range(4):
            k += 1
            f = env.flow("tcp", fw=g0, rev=g1, real=True)
            srv = env.spawn(env.sv, "tcpsrv", f.s, f.sport, 100, out=out(env, "srv%d" % k))
            env.sleep(0.3)
            cli = env.spawn(env.cl, "tcpcli", f.c, f.cport, f.s, f.sport, 100, 10,
                            out=out(env, "cli%d" % k))
            cli.wait()
            try:
                srv.wait(5)
            except subprocess.TimeoutExpired:
                kill(srv)
            r = read(out(env, "cli%d" % k))
            fast += r.startswith("OK") and int(r.split()[1]) < 500
        undo()
        if ms >= 5:
            env.check("server %d ms away: connections without retransmission (of 4)" % ms, fast, 4)
        else:
            env.ok("server %d ms away: %d of 4 connections without retransmission" % (ms, fast))


def push(env, gaps, count, timeout=30):
    """the server pushes 1 KiB after each gap into a connection the client
    only acknowledges; returns the client's result"""
    g0, g1 = env.g[:2]
    f = env.flow("tcp", fw=g0, rev=g1, real=True)
    env.spawn(env.sv, "tcppush", f.s, f.sport, gaps, "0")
    env.sleep(0.5)
    cli = env.spawn(env.cl, "tcpread", f.c, f.cport, f.s, f.sport, count, timeout, "0",
                    out=out(env, "cli"))
    cli.wait()
    return read(out(env, "cli"))


@scenario(gateways=2, requires=NO_ACK, tags={"tcp"})
def tcp_idle_push(env):
    """An idle asymmetric connection: the server pushes after 16 s of
    silence, longer than an element_timeout and the UDP timeout, well within
    the TCP timeout. g0 keeps announcing the idle flow, so g1 lets the push
    in."""
    undo = slow_server(env)
    env.start()
    r0 = rejects(env)
    env.check("3 chunks, the last after 16 s of silence", push(env, "0.5,0.5,16", 3), "^OK 3$")
    env.check("nothing rejected", rejects(env) - r0, 0)
    undo()


@scenario(gateways=2, requires=NO_ACK, tags={"tcp", "slow"})
def tcp_idle_limit(env):
    """The limit: silence beyond the TCP timeout (scaled 40 s; production
    7440 s) ends the flow on g0, the peers drop it an element_timeout later,
    and the server's next segment is reset."""
    undo = slow_server(env)
    env.start()
    gap = env.timers["tcp"] + env.E + 2 * env.I + 2
    res = push(env, "0.5,%d" % gap, 2, gap + 6)
    env.check("the push after the timeout does not arrive", res, "^FAIL after 1")
    env.check("it was rejected", rejects(env), lambda n: n >= 1)
    undo()


@scenario(gateways=3, requires=NO_ACK, tags={"tcp"})
def tcp_busy(env):
    """A busy asymmetric connection, 1 KiB each way every second for 25 s
    (almost 3 element_timeouts): it survives, nothing rejected."""
    g0, g1 = env.g[:2]
    undo = slow_server(env)
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1, real=True)
    env.spawn(env.sv, "tcpecho", f.s, f.sport, 40)
    env.sleep(0.5)
    cli = env.spawn(env.cl, "tcptalk", f.c, f.cport, f.s, f.sport, 25, 1, 10, out=out(env, "cli"))
    cli.wait()
    env.check("25 exchanges", read(out(env, "cli")), "^OK 25$")
    env.check("nothing rejected", rejects(env), 0)
    undo()
