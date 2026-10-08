"""TCP: the little state the egress program keeps (handshake seen, closing).
Real connections over asymmetric paths are gwlab's."""
from ..scenario import scenario
from .common import rejects, synced

EST, CLOSING = 1, 2


@scenario(gateways=2)
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


@scenario(gateways=2)
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
