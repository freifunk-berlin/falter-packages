"""Many flows at once."""
import os
import time

from ..scenario import scenario


@scenario(gateways=2, tags={"heavy", "slow"})
def scale(env):
    """SCALE_FLOWS (default 50000) new flows through g0 in one burst, faster
    than the event ring takes them: what the events miss, the next round
    announces. g1 holds all of them, replies pass, and the daemons' loops
    never stall."""
    n = int(os.environ.get("SCALE_FLOWS", 50000))
    g0, g1 = env.g[:2]
    env.start("--udp-timeout", 120, debug=False)
    g0.tick()
    b = env.bulk(n, fw=g0, rev=g1)
    t0 = time.monotonic()
    b.flood()
    env.ok("flood of %d flows took %.1f s" % (n, time.monotonic() - t0))
    env.wait_for("g1 holds all of them", 4 * env.I + 10,
                 lambda: (g1.st("remote") or 0) >= n, step=0.5)
    env.ok("after %.1f s; g0 events %s (ring overruns %s), g0 round %s ms, g1 round %s ms"
           % (time.monotonic() - t0, g0.st("tx_events"), g0.st("ev_overruns"),
              g0.st("refresh_ms"), g1.st("refresh_ms")))
    env.wait_for("g0 has walked them all as local flows", 3 * env.I + 5,
                 lambda: (g0.st("local") or 0) >= n, step=0.5)
    env.ok("g0 round %s ms for %s announced flows" % (g0.st("refresh_ms"),
                                                       g0.st("refresh_entries")))
    d0 = b.delivered("rev")
    for k in (0, n // 2, n - 1):
        port = b.first + k % (65536 - b.first)
        if k < 65536 - b.first:
            env.probe(env.sv, "send", b.s, b.sport, b.c, port)
    env.sleep(0.5)
    env.check("replies to sampled flows pass g1", b.delivered("rev") - d0, lambda v: v >= 1)
    g0.tick()
    g1.tick()
    env.check("no loop stalled (max ms per interval)",
              [g0.st("loop_max_ms"), g1.st("loop_max_ms")], lambda v: max(v) < 500)
    env.check("nothing refused or failed on g1",
              [g1.st("rx_limited"), g1.st("rx_errors")], [0, 0])
