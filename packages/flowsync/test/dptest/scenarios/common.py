"""Helpers shared by the scenarios."""
import os
import sys

TCX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tcx.py")


def tcx(g, *args):
    """the gateway's uplink hooks from outside the daemon (tcx.py): list,
    detach NAME..., fill; on wan0 ingress|egress"""
    return g.node.run(sys.executable, TCX, args[0], "wan0", *args[1:])


def hook_names(g, hook):
    """the names of the programs on the uplink's hook, in order"""
    return [ln.split()[1] for ln in tcx(g, "list", hook).splitlines() if ln.strip()]


def replies(env, f, n=3, gap=0.2, via=None):
    """n packets server -> client; how many came through"""
    d0 = f.delivered("rev")
    for _ in range(n):
        f.send("rev", via=via)
        env.sleep(gap)
    env.sleep(0.3)
    return f.delivered("rev") - d0


def rejects(env):
    return sum(g.fwc("rej") for g in env.g)


def has_remote(g, f):
    return lambda: g.ft(f).remote


def synced(env, f, others=None, timeout=1):
    """the flow's announcement reached the other gateways"""
    for g in others or [g for g in env.g if g is not f.fw]:
        env.wait_for("%s holds the flow of %s" % (g, f.fw), timeout, has_remote(g, f), step=0.05)


def slow_server(env, ms=20):
    """a server that is not next door: a reply never beats the announcement
    by being faster than any network; returns the undo"""
    for g in env.g:
        env.sv.run("tc", "qdisc", "add", "dev", "w%d" % g.i, "root", "netem", "delay",
                   "%dms" % ms)

    def undo():
        for g in env.g:
            env.sv.run("tc", "qdisc", "del", "dev", "w%d" % g.i, "root", check=False)
    return undo


def out(env, name):
    return os.path.join(env.dir, name)


def read(path):
    try:
        with open(path) as f:
            return f.read().strip() or "none"
    except OSError:
        return "none"
