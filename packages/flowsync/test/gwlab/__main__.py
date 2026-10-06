"""python3 -m gwlab SCENARIO --impl NAME [options]

  SCENARIO            a module in gwlab/scenarios (steady)
  --impl NAME         none | flowsync | flowsync_conntrack | conntrackd
  --set KEY=VALUE     an option of the implementation, repeatable (bin=..., see impl/*.py)
  --fleet NAME ..     only these fleets of the scenario (default: all, in parallel)
  --flow TEXT         only flows whose name contains TEXT (e.g. 'tcp_talk:A>gw1>B>gw2')
  --scale N           production's timers and the scenario's durations divided by N
                      (default: the scenario's, usually 10; 1 = production time)
  --out DIR           where results and logs go (default: a new directory in /tmp)

One lab per fleet, each in namespaces of its own: unprivileged
(`unshare -Urn`, in a systemd scope so that CPU can be counted per gateway),
or as they are if started by root (a VM), which some implementations need.
Exit status 1 if a flow failed.
"""
import argparse
import importlib
import json
import os
import subprocess
import sys
import tempfile
import time

from . import expect, report
from .impl import load
from .lab import Lab
from .scenario import grid, in_lab, span
from .timers import Timers

HERE = os.path.dirname(os.path.abspath(__file__))


def traffic(lb, flows, out, tag, during=None):
    """run the flows: one agent per endpoint. during(t0) may act while they
    run (t0: the second the flows count from). Returns what each end saw."""
    t0 = time.monotonic() + 4
    agents, parts = [], []
    for role, ends in (("server", lb.servers), ("client", lb.clients)):
        for e in ends.values():
            mine = [f for f in flows if f[role] == e.name]
            if not mine:
                continue
            base = os.path.join(out, "%s.%s" % (e.node.name, tag))
            with open(base + ".job.json", "w") as fh:
                json.dump(dict(t0=t0, flows=mine), fh)
            agents.append(e.node.spawn(sys.executable, os.path.join(HERE, "agent.py"), role,
                                       base + ".job.json", base + ".jsonl", log=base + ".log"))
            parts.append(base + ".jsonl")
    if during:
        during(t0)
    for a in agents:
        a.wait()
    seen = {"client": {}, "server": {}}
    for rf in parts:
        for line in open(rf):
            r = json.loads(line)
            seen[r["role"]][r["id"]] = r
    return seen


def measure_sync(lb, out, delays_ms=None):
    """the implementation's sync latency in this lab, seen from outside: new
    flows over every asymmetric gateway pair whose server holds its first
    answer back. Returns ms from a flow's first packet at the forward gateway
    until the return gateway is sure to know it (an upper bound: the path's
    own margin and the delay steps limit the resolution), or None if answers
    never passed."""
    c, s = next(iter(lb.clients)), next(iter(lb.servers))
    flows = [f for f in grid(["ladder"], [c], [s], list(lb.gw), list(lb.gw)) if f["fwd"] != f["rev"]]
    # an answer later than an unanswered UDP flow lives would not pass one gateway either
    limit = 800 * lb.timers.conntrack()["udp_timeout"]
    delays_ms = [d for d in (delays_ms or flows[0]["p"]["delays_ms"]) if d < limit]
    for f in flows:
        f["p"] = dict(f["p"], delays_ms=delays_ms)
    lb.place(flows)
    for n, f in enumerate(flows):
        f["start"] = 0.01 * n
    seen = traffic(lb, flows, out, "sync")
    lat = expect.sync_latency([dict(flow=f, client=seen["client"].get(f["id"])) for f in flows])
    if not lat or lat["all_from_ms"] is None:
        return None, lat
    return flows[0]["margin_ms"] + lat["all_from_ms"], lat


def lab(args, opts):
    """inside the namespaces: one fleet, start to finish"""
    sc = importlib.import_module(".scenarios." + args.scenario, __package__)
    impl = load(args.impl, opts)
    fleet = sc.FLEETS[args.fleet[0]]
    out = os.path.join(args.out, args.fleet[0])
    timers = Timers(args.scale)
    alone = getattr(sc, "ALONE", False)
    events = [dict(e, at=timers.span(e["at"]), seconds=timers.span(e.get("seconds", 0)))
              for e in getattr(sc, "EVENTS", [])]
    lb = Lab(sc.TOPOLOGY, fleet, out, timers)
    try:
        for g in lb.gw.values():
            if g.policy["offload"] and not impl.offload:
                g.policy["offload"] = False
            impl.install(g)
        for g in lb.gw.values():
            impl.start(g)
        time.sleep(2)                           # the implementations settle
        # verdicts depend on how fast the implementation syncs: measure it first
        sync_ms, ladder = (None, None) if alone else measure_sync(lb, out, sc.TOPOLOGY.get("ladder_ms"))
        flows = [dict(f, p=in_lab(f["p"], timers)) for f in sc.FLOWS if args.flow in f["id"]]
        lb.place(flows)
        at = 0.0
        for n, f in enumerate(flows):
            f["start"] = at if alone else 0.01 * n      # one after the other / not all in the same instant
            at += span(f["p"]) + 2
        cpu0 = {n: g.cpu_ms() for n, g in lb.gw.items()}
        windows = {}
        shots = []                              # (seconds after t0, {(source, destination): packets bypassed})

        def bypassed(t0):
            total = {}
            for g in lb.gw.values():
                for k, n in impl.bypassed(g).items():
                    total[k] = total.get(k, 0) + n
            shots.append((time.monotonic() - t0, total))
        # the stateless accept may carry a flow where the race cannot be won: at
        # its start and around an event. In between it must not be needed:
        # look at its counters when things have settled and before the next event
        settle = (sync_ms or 0) / 1000 + 1      # the sync, and the endpoints' first retry
        looks = [max(f["start"] for f in flows) + settle] if flows else []
        for e in sorted(events, key=lambda e: e["at"]):
            looks += [e["at"] - 0.3, e["at"] + e["seconds"] + settle]

        def during(t0):
            todo = []                           # (seconds after t0, what happens)
            for e in events:
                g = lb.gw.get(e.get("gw"))
                if e["do"] == "reroute":
                    todo.append((e["at"], lambda e=e: lb.reroute(flows, e["leg"])))
                elif e["do"] == "lose_state":
                    todo.append((e["at"], lambda g=g: impl.lose_state(g)))
                elif e["do"] == "restart":
                    todo.append((e["at"], lambda g=g: (impl.stop(g), impl.start(g))))
                elif e["do"] == "uplink_recreate":
                    todo.append((e["at"], lambda g=g: lb.uplink_recreate(g)))
                elif e["do"] == "sync_blackout":
                    todo.append((e["at"], lambda g=g: lb.sync_blackout(g, True)))
                    todo.append((e["at"] + e["seconds"], lambda g=g: lb.sync_blackout(g, False)))
                else:
                    raise SystemExit("unknown event %r" % e["do"])
            if not alone:
                todo += [(at, lambda: bypassed(t0)) for at in looks]
            for at, act in sorted(todo, key=lambda x: x[0]):
                time.sleep(max(0, t0 + at - time.monotonic()))
                act()
            if alone:                           # CPU per gateway while each flow ran
                for f in flows:
                    time.sleep(max(0, t0 + f["start"] - time.monotonic()))
                    a = {n: (g.cpu_ms(), g.kernel_ms()) for n, g in lb.gw.items()}
                    time.sleep(max(0, t0 + f["start"] + span(f["p"]) - time.monotonic()))
                    windows[f["id"]] = {
                        n: dict(cpu=[round(x - y) for x, y in zip(g.cpu_ms(), a[n][0])] if a[n][0] else None,
                                kernel=round(g.kernel_ms() - a[n][1]) if a[n][1] is not None else None)
                        for n, g in lb.gw.items()}

        t_first = [0]
        seen = traffic(lb, flows, out, "flows", lambda t0: (t_first.__setitem__(0, t0), during(t0)))
        if not alone:
            bypassed(t_first[0])
        gws = {}
        for n, g in lb.gw.items():
            c1 = g.cpu_ms()
            gws[n] = dict(policy=g.policy, sync_tx=g.sync_traffic(),
                          cpu_ms=[c1[0] - cpu0[n][0], c1[1] - cpu0[n][1]] if c1 and cpu0[n] else None)
        for g in lb.gw.values():
            impl.stop(g)
    finally:
        lb.close()
    res = []
    for f in flows:
        c, s = seen["client"].get(f["id"]), seen["server"].get(f["id"])
        bad, retry, recovery = expect.judge(f, c, s, events, sync_ms)
        # the quiet stretches: from a look after things settled to the next look before an event
        key, quiet = (f["s"], f["c"]), []
        for (ta, a), (tb, b) in zip(shots[0::2], shots[1::2]):
            if tb > ta and b.get(key, 0) > a.get(key, 0):
                quiet.append((round(ta), round(tb), b.get(key, 0) - a.get(key, 0)))
        bad += expect.bypass(f, quiet, sync_ms)
        res.append(dict(flow=f, client=c, server=s, bad=bad, retry=retry, recovery=recovery,
                        bypassed=shots[-1][1].get(key, 0) if shots else 0,
                        cpu_ms=windows.get(f["id"])))
    with open(os.path.join(out, "results.json"), "w") as fh:
        json.dump(dict(scenario=args.scenario, impl=args.impl, fleet=args.fleet[0], dir=out,
                       alone=alone, events=len(events), scale=timers.scale, clamped=timers.clamped,
                       sync_ms=sync_ms, ladder=ladder, gateways=gws, flows=res), fh, indent=1)
    return 0


def check_scale(scale, impl):
    """warn where the scale stops being exact, refuse where it stops being a test"""
    def broken(n):
        t = Timers(n)
        return t.broken() + impl.broken(t), t
    why, t = broken(scale)
    if why:
        usable = max(n for n in range(1, int(scale) + 1) if not broken(n)[0])
        raise SystemExit("scale %g is useless for this implementation (highest usable: %d):\n  %s"
                         % (scale, usable, "\n  ".join(why)))
    if scale > 10:
        print("warning: scale %g is above 10, timers no longer keep production's ratios exactly:" % scale)
        for names, what in ((t.clamped, "held at 1 s"), (t.rounded, "rounded to whole seconds")):
            if names:
                print("  %s: %s" % (what, ", ".join("%s (%g s)" % kv for kv in names.items())))
        print("  and whatever is real time (sync latency, TCP retries) weighs %g times heavier "
              "than in production" % scale, flush=True)


def main():
    ap = argparse.ArgumentParser(prog="gwlab", usage=__doc__)
    ap.add_argument("scenario")
    ap.add_argument("--impl", required=True)
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--fleet", nargs="*", default=[])
    ap.add_argument("--flow", default="")
    ap.add_argument("--scale", type=float)
    ap.add_argument("--out")
    ap.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()
    opts = dict(kv.split("=", 1) for kv in args.set)
    if args.inside:
        return lab(args, opts)

    sc = importlib.import_module(".scenarios." + args.scenario, __package__)
    impl = load(args.impl, opts)                # fails early on a missing binary
    root = os.geteuid() == 0
    if impl.needs_root and not root:
        raise SystemExit("%s needs real root: run it in a VM" % args.impl)
    args.out = os.path.abspath(args.out or tempfile.mkdtemp(prefix="gwlab."))
    fleets = args.fleet or list(sc.FLEETS)
    alone = getattr(sc, "ALONE", False)
    args.scale = args.scale or getattr(sc, "SCALE", 10)
    if not alone:
        check_scale(args.scale, impl)
    spans = [span(in_lab(f["p"], Timers(args.scale))) for f in sc.FLOWS if args.flow in f["id"]]
    secs = (sum(spans) + 2 * len(spans) + 15) * len(fleets) if alone else max(spans) + 35
    print("%s on %s: fleets %s, about %d s; results in %s"
          % (args.scenario, args.impl, ", ".join(fleets), secs, args.out), flush=True)
    wrap = ["unshare", "-n"] if root else ["unshare", "-Urn"]
    if not root and subprocess.run(["systemd-run", "--user", "--scope", "-q", "true"],
                                   capture_output=True).returncode == 0:
        wrap = ["systemd-run", "--user", "--scope", "-q", "-p", "Delegate=yes"] + wrap
    procs = []
    for fl in fleets:
        os.makedirs(os.path.join(args.out, fl), exist_ok=True)
        argv = wrap + [sys.executable, "-m", "gwlab", args.scenario, "--inside", "--impl", args.impl,
                       "--fleet", fl, "--flow", args.flow, "--out", args.out, "--scale", str(args.scale)]
        for kv in args.set:
            argv += ["--set", kv]
        log = open(os.path.join(args.out, fl, "lab.log"), "w")
        procs.append((fl, subprocess.Popen(argv, cwd=os.path.dirname(HERE), stdout=log, stderr=log)))
        if alone:
            procs[-1][1].wait()                 # measurements: one lab at a time
    failed = 0
    for fl, p in procs:
        rc = p.wait()
        path = os.path.join(args.out, fl, "results.json")
        if rc or not os.path.exists(path):
            print("== fleet %s: the lab itself failed, see %s ==\n" % (fl, os.path.join(args.out, fl, "lab.log")))
            failed += 1
            continue
        run = json.load(open(path))
        print(report.render(run) + "\n", flush=True)
        failed += sum(1 for r in run["flows"] if r["bad"])
    print("%s: %d failed" % ("FAIL" if failed else "PASS", failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
