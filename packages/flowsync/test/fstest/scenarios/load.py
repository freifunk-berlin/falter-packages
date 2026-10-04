"""Many flows: event socket overruns, slow and stuck rounds, flush recovery,
scale."""
from ..scenario import scenario


def marked(g, n):
    return lambda: g.count("udp", "marked") == n


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
    """conntrack -F on g1 while it holds 20000 copies: the resync request
    brings every copy back within seconds, not at g0's next round (interval
    10 s here, so a regular round cannot explain it)."""
    g0, g1 = env.g[:2]
    n = 20000
    opts = ("-r", 5000, "-i", 10, "-t", 30)
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


@scenario(gateways=3, once=True, tags={"load", "slow", "heavy"})
def scale(env):
    """100000 flows: the streaming dump against the small queue, the event
    socket under the burst, the per-tuple table, the copies over several
    rounds."""
    g0, g1, g2 = env.g[:3]
    n = 100000
    env.start("-r", 5000, "-C", n + n // 5)
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
