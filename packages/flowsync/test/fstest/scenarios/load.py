"""Many flows: event socket overruns, slow and stuck rounds, flush recovery,
scale."""
import time

from ..scenario import scenario


def marked(g, n):
    return lambda: g.count("udp", "marked") == n


def back_after(g, n, limit):
    """seconds until g holds n UDP copies again, None if not within limit"""
    t0 = time.monotonic()
    while time.monotonic() - t0 < limit:
        if g.count("udp", "marked") >= n:
            return time.monotonic() - t0
        time.sleep(0.1)
    return None


@scenario(gateways=2, once=True, tags={"load", "heavy"})
def enobufs(env):
    """The event socket overruns under a burst of 2000 new flows. The daemon
    does not resubscribe (that would throw the queue away), and a round repairs
    the gap: every flow ends up on g1."""
    g0, g1 = env.g[:2]
    env.start("-B", 8192, "-r", 100)
    b = env.bulk(2000, fw=g0, rev=g1, first=40000)
    b.flood()
    env.wait_for("g1 holds all 2000 copies", 2 * env.I + 4, marked(g1, 2000))
    env.wait_st("g0 reports event socket overruns", g0, "ev_overruns", 1)
    env.true("g0 did not resubscribe", "resubscribing" not in g0.log())
    env.ok("g0: ev_recv=%s ev_overruns=%s tx_events=%s tx_refresh=%s; g1: created=%s"
           % (g0.st("ev_recv"), g0.st("ev_overruns"), g0.st("tx_events"), g0.st("tx_refresh"),
              g1.st("inject_created")))


@scenario(gateways=2, once=True, tags={"load", "heavy"})
def enobufs_early(env):
    """NEW events lost to an overrun are announced by an early round, not by the
    next regular one (interval 10 s here)."""
    g0, g1 = env.g[:2]
    opts = ("-i", 10, "-t", 30, "-r", 100)
    env.start(*opts)
    g0.restart(*opts, "-B", 8192)
    env.wait_for("g0 restarted with a small event buffer", 8, g0.up)
    g0.tick()
    b = env.bulk(2000, fw=g0, rev=g1, first=40000)
    b.flood()
    env.wait_for("g1 holds all 2000 copies within 3 s, not at g0's next round", 3,
                 marked(g1, 2000))


@scenario(gateways=2, once=True, tags={"load", "heavy", "repro"})
def resync_bulk(env):
    """conntrack -F on g1 while it holds 6000 copies, with a slow refresh
    (tx_rate 50: 1500 records/s per peer, a 4 s round). The resync answer
    is sent faster than the regular rounds: every copy is back within 2.5 s."""
    g0, g1 = env.g[:2]
    n = 6000
    env.start("-r", 50, "-i", 10, "-t", 30, "-c", n, debug=False)
    g0.set_sysctl(udp_timeout=300)
    b = env.bulk(n, fw=g0, rev=g1)
    b.flood()
    env.wait_for("g1 holds all %d copies" % n, 30, marked(g1, n))
    g0.tick(timeout=15)
    env.sleep(5)                # g0's regular round is sent, the next is 5 s away
    g1.flush()
    t = back_after(g1, n, 4.5)
    env.true("every copy is back within 2.5 s of the flush", t is not None and t <= 2.5,
             "after %s s" % ("%.1f" % t if t is not None else "> 4.5"))


@scenario(gateways=2, once=True, tags={"load", "heavy"})
def overrun(env):
    """tx_rate too low for the table: a round takes longer than the interval.
    The next round is delayed, nothing is dropped, the copies still arrive."""
    g0, g1 = env.g[:2]
    env.start("-r", 2)
    b = env.bulk(300, fw=g0, rev=g1, first=42000)
    b.flood()
    env.wait_for("g1 holds all 300 copies", 2 * env.I + 4, marked(g1, 300))
    env.wait_st("g0 reports a delayed round", g0, "refresh_overrun", 1)
    env.check("g0 dropped no refresh entries", g0.st("tx_refresh_dropped"), 0)
    env.check("g0's rounds completed", g0.st("refresh_rounds"), lambda n: n >= 1)


@scenario(gateways=2, once=True, tags={"load", "heavy"})
def stuck_dump(env):
    """A round whose sender is held back by its own queue is slow, not stuck:
    it is not aborted."""
    g0, g1 = env.g[:2]
    env.start("-r", 1, "-l", 1)
    g0.set_sysctl(udp_timeout=300)
    b = env.bulk(2000, fw=g0, rev=g1)
    b.flood()
    env.sleep(4 * env.I + 1)
    env.check("a slow round is not aborted as stuck", g0.st("refresh_errors"), 0)


@scenario(gateways=2, once=True, tags={"load", "heavy"})
def flush_retry(env):
    """conntrack -F on g1 while it holds 10000 copies: the resync request
    brings every copy back within seconds, not at g0's next round (interval
    10 s here, so a regular round cannot explain it)."""
    g0, g1 = env.g[:2]
    n = 10000
    # all flows come from one client address: lift the per-client limit
    opts = ("-r", 5000, "-i", 10, "-t", 30, "-c", n)
    env.start(*opts, debug=False)
    g0.set_sysctl(udp_timeout=300)
    b = env.bulk(n, fw=g0, rev=g1)
    b.flood()
    env.wait_for("g1 holds all %d copies" % n, 30, marked(g1, n))
    g0.tick(timeout=15)         # g0's next regular round is ~10 s away
    g1.flush()
    series = []
    for _ in range(12):
        series.append(g1.count("udp", "marked"))
        env.sleep(0.5)
    env.ok("copies on g1 every 0.5 s after the flush: %s" % " ".join(map(str, series)))
    back = next((k for k, x in enumerate(series) if x == n), None)
    env.true("every copy is back within 3 s of the flush", back is not None and back <= 6,
             "after %s s" % (back / 2 if back is not None else "-"))
    # the re-created copies' NEW events stay in the kernel (mark filter): they
    # would otherwise overrun g1's event socket and drop real flows' events
    g1.tick(timeout=15)
    env.check("none of g1's own copies reached its event socket", g1.st("ev_own"), 0)
    env.check("g1's event socket did not overrun", g1.st("ev_overruns"), 0)


@scenario(gateways=2, once=True, tags={"load", "heavy"})
def flush_stale_chunk(env):
    """conntrack -F on g1 while its copies phase is held back by its own send
    queue. The dump chunk the kernel generated before the flush is read after
    the DESTROY events and still shows copies that are gone; g1 must not take
    them for its own again. Once sync works, every copy comes back with g0's
    next announcement, and none stays missing.

    g1 holds 3000 copies that just saw a packet: they are announced from
    evidence, fill g1's queue, and tx_rate 6 (180 records a second) keeps a
    generated chunk waiting in the dump socket for seconds. element_timeout
    60 s and a 120 s UDP stream timeout make the copies live on their traffic
    (held, not refreshed: a refresh would find a gone copy and re-create it).
    Sync from g0 is blocked around the flush, so that g0's resync answer
    cannot re-create the copies before the stale chunk is read."""
    g0, g1 = env.g[:2]
    n = 3000
    g0.start("-t", 60, debug=False)
    g1.start("-t", 60, "-r", 6, debug=False)
    env.wait_for("daemons up", 8, lambda: g0.up() and g1.up())
    g0.set_sysctl(udp_timeout=300)
    g1.set_sysctl(udp_timeout_stream=120)
    b = env.bulk(n, fw=g0, rev=g1)
    b.flood()
    env.wait_for("g1 holds all %d copies" % n, 2 * env.I + 4, marked(g1, n))
    b.flood(via=g1)             # a packet on every copy: ASSURED, 120 s, evidence
    g1.tick()                   # the round that announces them is running
    g1.block_sync(g0)
    g1.flush()
    env.sleep(3)                # g1 reads the stale chunk meanwhile
    g1.unblock_sync()
    series = []
    while len(series) < 2 * env.I + 2 and n not in series:
        env.sleep(1)
        series.append(g1.count("udp", "marked"))
    env.ok("copies on g1 every second after sync resumed: %s" % " ".join(map(str, series)))
    env.true("every copy is back within two of g0's rounds", n in series[:2 * env.I + 2],
             "%d missing" % (n - max(series)))
    env.wait_for("no copy stays missing", 30, marked(g1, n))
    env.ok("g1: owned=%s copies=%s copies_lost=%s inject_created=%s"
           % (g1.st("owned"), g1.st("copies"), g1.st("copies_lost"), g1.st("inject_created")))


@scenario(gateways=3, once=True, tags={"load", "slow", "heavy"})
def scale(env):
    """100000 flows: the streaming dump against the small queue, the event
    socket under the burst, the per-tuple table, the copies over several
    rounds."""
    g0, g1, g2 = env.g[:3]
    n = 100000
    env.start("-r", 5000, "-C", n + n // 5, "-c", n)   # one client address
    g0.set_sysctl(udp_timeout=300)
    b = env.bulk(n, fw=g0, rev=g1)
    b.flood()
    env.wait_for("g0 tracks all %d natives" % n, 20, lambda: g0.count("udp", "native") == n)
    env.wait_for("g1 holds all %d copies" % n, 5 * env.I, marked(g1, n))
    env.wait_for("g2 holds all %d copies" % n, 2 * env.I, marked(g2, n))
    env.hold("g1 keeps all %d copies over the following rounds" % n, 4 * env.I, marked(g1, n),
             step=env.I)
    env.check("no unmarked entry on g1: a refresh never creates", g1.count("udp", "native"), 0)
    env.check("g0 dropped no refresh entries", g0.st("tx_refresh_dropped"), 0)
    env.check("g0 had no refresh errors", g0.st("refresh_errors"), 0)
    env.check("g0's last round queued all natives", g0.st("refresh_entries"), n)
    env.check("g1 had no injection errors", g1.st("inject_errors"), 0)
    env.check("g1 owns all copies", g1.st("owned"), n)
    env.ok("g0: refresh_ms=%s loop_max_ms=%s; g1: rx_evictions=%s loop_max_ms=%s"
           % (g0.st("refresh_ms"), g0.st("loop_max_ms"), g1.st("rx_evictions"),
              g1.st("loop_max_ms")))
    g0.flush()
    env.wait_for("copies gone once g0's natives are gone", env.E + 3 * env.I, marked(g1, 0))
