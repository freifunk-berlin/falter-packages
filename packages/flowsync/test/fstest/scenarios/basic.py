"""Basic sync, expiry, reroute, two origins, loss, MTU."""
from ..scenario import scenario
from .common import all_gone, gone, native_unreplied


@scenario(gateways=3, tags={"basic"})
def basic(env):
    """One flow, forward via g0, a reply via g1. Copies appear on g1 and g2,
    turn ASSURED from a packet, a copy with traffic announces itself, and no
    announcement touches g0's native entry."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(8, 2, f.send)
    env.wait_for("g1 has the copy", 3, lambda: g1.ct(f).is_copy)
    env.check("g1 copy is marked and SEEN_REPLY", g1.ct(f), lambda e: e.is_copy and e.seen_reply)
    env.true("g1 copy not ASSURED before any reply", not g1.ct(f).assured, g1.ct(f))
    env.check("g2 has the copy too", g2.ct(f), lambda e: e.is_copy and e.seen_reply)
    env.check("g0 native is unreplied and unmarked", g0.ct(f), native_unreplied)
    env.check("the forward packets reached the server", f.delivered("fwd"), lambda n: n >= 1)
    g1.block_sync()
    f.send("rev")
    env.wait_for("the reply reached the client through g1", 3, lambda: f.delivered("rev") >= 1)
    env.wait_for("g1 copy turns ASSURED after one reply", 3, lambda: g1.ct(f).assured)
    env.check("g1 copy carries traffic (evidence)", g1.ct(f), lambda e: e.carries_traffic(env.E))
    env.wait_st("g1 announced its copy from traffic evidence", g1, "tx_copies", 1)
    env.wait_st("g0 saw EEXIST from g1's announcement", g0, "inject_exists", 1)
    env.check("g0 native untouched by it", g0.ct(f), native_unreplied)
    g1.unblock_sync()
    env.sleep(env.I + 1)
    env.check("g1 copy still there and ASSURED", g1.ct(f), lambda e: e.is_copy and e.assured)
    env.check("g2 copy still present", g2.ct(f), lambda e: e.alive)
    lp.wait()


@scenario(gateways=3, tags={"basic"})
def dead_flow(env):
    """The flow dies. Nothing is kept alive by announcements: g0's native
    expires on its natural timeout, the copies soon after, and the
    announcements stop."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(5, 2, f.send)
    env.sleep(1.5)
    f.send("rev")
    lp.wait()
    env.wait_for("g0 native expires on its natural timeout", env.udp_life(g0) + 2,
                 lambda: gone(g0, f))
    env.wait_for("g1 copy gone", env.udp_life(g1, True) + env.E + 2 * env.I, lambda: gone(g1, f))
    env.wait_for("g2 copy gone", env.E + 3 * env.I, lambda: gone(g2, f))
    tb = [g.st("tx_refresh") for g in (g0, g1, g2)]
    env.sleep(2 * env.I + 1)
    env.check("no gateway announces once everything is dead",
              [g.st("tx_refresh") for g in (g0, g1, g2)], tb)


@scenario(gateways=3, tags={"basic"})
def reroute(env):
    """The forward path moves away from g0 while the flow lives on: only g1 sees
    packets now. g1's copy announces itself from traffic evidence, g2 keeps its
    copy, g0 learns the flow back as a copy, and all of it is gone once the
    traffic stops."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(4, 2, f.send)
    env.sleep(1.5)
    f.send("rev")
    lp.wait()
    env.wait_for("g0 native expires", env.udp_life(g0) + 4, lambda: gone(g0, f))
    rl = env.loop(8, 2, lambda: f.send("rev"))
    env.hold("g2 keeps its copy while only g1 sees traffic", 15, lambda: g2.ct(f).alive, step=2)
    env.check("g1 announced its copy from traffic evidence", g1.st("tx_copies"), lambda n: n >= 1)
    env.check("g0 learned the flow back as a copy from g1", g0.ct(f),
              lambda e: e.is_copy and e.seen_reply)
    rl.wait()
    env.wait_for("everything gone after the traffic stops",
                 env.udp_life(g1, True) + env.E + 3 * env.I, all_gone(env.g[:3], f))


@scenario(gateways=3, tags={"basic"})
def two_origins(env):
    """Two gateways forward the same flow (the forward path is split). Both hold
    it natively, both announce it, and neither one's announcement touches the
    other's native entry. Announcements between g0 and g2 are held off until
    both natives exist."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    g0.block_sync(g2)
    g2.block_sync(g0)
    lp = env.loop(8, 2, lambda: (f.send(via=g0), f.send(via=g2)))
    env.wait_for("g1 has the copy", 3, lambda: g1.ct(f).is_copy)
    env.check("g0 native unreplied and unmarked", g0.ct(f), native_unreplied)
    env.check("g2 native unreplied and unmarked", g2.ct(f), native_unreplied)
    g0.unblock_sync()
    g2.unblock_sync()
    env.wait_st("g0 refused g2's announcement (EEXIST)", g0, "inject_exists", 1)
    env.wait_st("g2 refused g0's announcement (EEXIST)", g2, "inject_exists", 1)
    env.check("g0 native still unreplied and unmarked", g0.ct(f), native_unreplied)
    env.check("g2 native still unreplied and unmarked", g2.ct(f), native_unreplied)
    env.check("g1 copy present", g1.ct(f), lambda e: e.is_copy and e.seen_reply)
    lp.wait()
    env.wait_for("everything gone after both stop", env.udp_life(g0) + env.E + 3 * env.I,
                 all_gone(env.g[:3], f))


@scenario(gateways=3, tags={"basic"})
def loss(env):
    """g1 loses every third record datagram, including the very first one.
    element_timeout is three intervals, so one lost refresh never costs a copy."""
    g0, g1, g2 = env.g[:3]
    env.start()
    g1.loss(3)
    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(15, 2, f.send)
    env.sleep(env.I + 2)
    env.hold("g1 copy survives periodic announcement loss", 22, lambda: g1.ct(f).alive, step=2)
    lp.wait()
    env.sleep(env.I)
    per_peer = (g0.st("tx_datagrams") or 0) // (len(env.g) - 1) or 1
    env.check("g2 received all of g0's datagrams (percent)",
              100 * (g2.st("rx_datagrams") or 0) // per_peer, lambda p: 85 <= p <= 119)
    env.check("g1 received about two thirds (percent)",
              100 * (g1.st("rx_datagrams") or 0) // per_peer, lambda p: 55 <= p <= 79)
    env.check("g1 never had to create the copy again", g1.st("inject_created"), 1)


@scenario(gateways=3, tags={"basic"})
def sparse_udp(env):
    """A UDP flow carried on a copy (g2) with a packet period between
    element_timeout and the stream timeout: the reply gateway's copy must be
    there at every reply. (Did not reproduce as a break; kept as a guard.)"""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    f.send()
    env.sleep(0.5)
    f.send("rev")
    env.wait_for("g2 has the copy", 3, lambda: g2.ct(f).alive)
    miss = 0
    for _ in range(4):
        f.send(via=g2)          # g2 forwards on its copy now
        env.sleep(1)
        if not g1.ct(f).alive:
            miss += 1
        f.send("rev")
        env.sleep(13)
    env.check("g1 had the copy at every reply (misses of 4)", miss, 0)


@scenario(gateways=2, tags={"basic"})
def mtu(env):
    """A path that loses sync packets above 1280 bytes: the default datagram
    (30 records, 1232 bytes as IPv4) fits, so refreshes survive."""
    g0, g1 = env.g[:2]
    env.start()
    g1.mtu_blackhole(1280)
    g0.set_sysctl(udp_timeout=300)
    b = env.bulk(100, fw=g0, rev=g1, first=45000)
    b.burst()
    env.wait_for("g1 got all 100 copies", 4, lambda: g1.count("udp", "marked") == 100)
    env.hold("g1 keeps all 100 copies over the refresh rounds", 4 * env.I,
             lambda: g1.count("udp", "marked") == 100)
