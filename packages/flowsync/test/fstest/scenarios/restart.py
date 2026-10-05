"""Daemon restarts, flushes, ownership, resync, peer liveness."""
import re
import time

from ..scenario import scenario
from .common import long_lived, native_unreplied


@scenario(gateways=2, tags={"restart"})
def restart(env):
    """g1's daemon restarts under a live flow. The copy survives in the kernel,
    ownership is re-learned from the mark, refreshes resume: no gap."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(14, 2, f.send)
    env.wait_for("g1 has the copy", 3, lambda: g1.ct(f).is_copy)
    env.hold("g1 copy present before the restart", 6, lambda: g1.ct(f).alive)
    g1.restart()
    env.hold("g1 copy never disappears across the restart", 18, lambda: g1.ct(f).alive)
    lp.wait()
    env.check("g1 re-learned ownership", g1.st("owned"), lambda n: n >= 1)
    env.check("few EEXIST during the handover", g1.st("inject_exists"), lambda n: n <= 3)


@scenario(gateways=2, tags={"restart"})
def flush(env):
    """conntrack -F on g1: the copy vanishes and comes back at once (the
    DESTROY events trigger a resync request). Interval 10 s here, so that
    neither gateway's regular round can explain it."""
    g0, g1 = env.g[:2]
    env.start("-i", 10, "-t", 30)
    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(10, 2, f.send)
    env.wait_for("g1 has the copy", 3, lambda: g1.ct(f).is_copy)
    g0.tick(timeout=15)             # g0's next regular round is ~10 s away
    env.sleep(1)
    rs = g1.st("tx_resync") or 0
    g1.flush()
    env.wait_for("copy re-created within 2 s (resync)", 2, lambda: g1.ct(f).is_copy)
    lp.wait()
    env.wait_for("g1 sent a resync request", 2 * 10 + 2,
                 lambda: (g1.st("tx_resync") or 0) > rs)
    env.check("g1 counted the lost copy", g1.st("copies_lost"), lambda n: n >= 1)
    env.wait_for("g1 owns the copy again", 2 * 10 + 2, lambda: (g1.st("owned") or 0) >= 1)


@scenario(gateways=2, once=True, tags={"restart"})
def expiry_resume(env):
    """A flow idles until g1's copy has expired, then resumes on the same
    5-tuple. g0 announces its new native at once (NEW event); g1 must create
    the copy from that announcement, not a round or two later (the DESTROY of
    an expired copy carries no timeout, but it tells g1 the copy is gone).
    Interval 10 s here."""
    g0, g1 = env.g[:2]
    env.start("-i", 10, "-t", 30)
    f = env.flow("udp", fw=g0, rev=g1)
    f.send()
    env.wait_for("g1 has the copy", 3, lambda: g1.ct(f).is_copy)
    # g0's native expires after the UDP timeout and g0 stops announcing it;
    # g1's copy runs out within element_timeout plus a round after that
    env.wait_for("g1's copy expired", env.timers["udp"] + 30 + 10 + 5,
                 lambda: not g1.ct(f).alive, step=0.1)
    f.send()
    env.wait_for("g1 creates the copy again from g0's announcement", 2,
                 lambda: g1.ct(f).is_copy)


@scenario(gateways=3, tags={"restart", "tcp"})
def restart_idle_copy(env):
    """After a restart the per-tuple table is empty; a copy seen for the first
    time is no evidence, so an idle copy whose last packet is long past is not
    announced again."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("tcp", fw=g2, rev=g1)
    f.tcp("fwd", "PA", 2000, 7000, 10)            # g2 forwards first: native on g2
    env.wait_for("g0 has the copy", 4, lambda: g0.ct(f).is_copy)
    f.tcp("fwd", "PA", 2010, 7000, 10, via=g0)    # then one packet on g0's copy
    env.check("g0's copy is ASSURED with a long timeout", g0.ct(f), long_lived)
    g2.flush()                                    # nobody else announces the flow any more
    env.sleep(2 * env.I + 1)
    a0 = g0.st("tx_copies")
    env.sleep(2 * env.I)
    env.check("g0 went silent after the round following the packet", g0.st("tx_copies"), a0)
    g0.restart()
    env.wait_for("g0 restarted", 8, g0.up)
    env.sleep(2 * env.I + 1)
    env.check("after the restart g0 announces nothing for the idle copy", g0.st("tx_copies"), 0)


@scenario(gateways=2, tags={"restart"})
def stale_own(env):
    """g1's copy is flushed and g1 then forwards the flow itself (a native).
    g0's next announcement must not touch that native: ownership was dropped
    by the DESTROY or NEW event, the announcement is an EXCL create."""
    g0, g1 = env.g[:2]
    env.start()
    g1.set_sysctl(udp_timeout=30)
    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(8, 1.5, f.send)
    env.wait_for("g1 has the copy", 3, lambda: g1.ct(f).is_copy)
    g1.tick()
    g1.block_sync(g0)       # or the resync after the flush re-creates the copy first
    g1.flush()
    f.send(via=g1)          # g1 forwards the flow itself
    env.check("g1's entry is native: unmarked, unreplied, 30 s", g1.ct(f),
              lambda e: native_unreplied(e) and e.timeout >= 29)
    g1.unblock_sync()
    env.sleep(env.I + 1)
    env.check("g0's announcement left g1's native alone", g1.ct(f),
              lambda e: native_unreplied(e) and 20 <= e.timeout <= 29)
    lp.wait()


@scenario(gateways=2, tags={"restart"})
def resync(env):
    """After a daemon restart the surviving copies are refreshed at once (the
    restarted daemon asks its peers for a round), and after a reboot (empty
    table) the copies are back within seconds. Interval 10 s here."""
    g0, g1 = env.g[:2]
    opts = ("-i", 10, "-t", 30)
    env.start(*opts)
    t0 = time.monotonic()
    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(25, 1.5, f.send)
    env.wait_for("g1 has the copy", 3, lambda: g1.ct(f).is_copy)
    # restart more than interval/2 after g1's last round: the copy is then due
    # for a refresh (a fresher one is rightly left alone), and g0 serves the
    # resync request (at most once per interval/2 from the same gateway)
    g1.tick(timeout=15)
    env.sleep(6)
    env.sleep(max(0, 10 / 2 + 1.5 - (time.monotonic() - t0)))
    g1.restart()
    env.wait_for("g1 restarted", 8, g1.up)
    # restart, first own round, request, g0's round, g1's own round: seconds,
    # where without resync the next refresh would come at g1's next tick (10 s);
    # rounds take seconds under load
    env.wait_for("daemon restart: g1's copy is refreshed within 7 s", 7,
                 lambda: g1.ct(f).offloaded or g1.ct(f).timeout >= 28)
    g0.tick()
    env.sleep(1)
    g1.stop()
    g1.flush()
    g1.start(*opts)
    env.wait_for("reboot (empty table): g1 has the copy again within 3 s", 3,
                 lambda: g1.ct(f).is_copy)
    lp.wait()


@scenario(gateways=3, tags={"restart"})
def peer_liveness(env):
    """A dead peer is visible: the status shows per peer when it was last
    heard, and a peer silent for three intervals is logged, as is its return."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("udp", fw=g2, rev=g1)
    f.send()                # g2 has a flow, so it announces
    env.wait_for("g0 heard from g2", 3, lambda: g0.ct(f).alive)
    g2.stop()
    env.sleep(4 * env.I + 1)
    line = g0.status_line("peer %s " % g2.addr)
    m = re.match(r"peer \S+ rx (\d+) age (\d+)( |$)", line)
    env.true("g0's status shows g2 silent for three intervals",
             bool(m) and int(m.group(1)) >= 1 and int(m.group(2)) >= 3 * env.I, line)
    env.check("g0 logged it", g0.log().count("peer %s: nothing received" % g2.addr), 1)
    g2.start()
    env.wait_for("g0 logs g2 back after its first heartbeat", 2 * env.I,
                 lambda: "peer %s: back" % g2.addr in g0.log())
