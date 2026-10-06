"""python3 -m gwlab SCENARIO --impl NAME [options]

  SCENARIO            a module in gwlab/scenarios (steady)
  --impl NAME         none | flowsync | conntrackd | flowsync_bpf
  --set KEY=VALUE     an option of the implementation, repeatable (bin=..., see impl/*.py)
  --fleet NAME ..     only these fleets of the scenario (default: all, in parallel)
  --flow TEXT         only flows whose name contains TEXT (e.g. 'tcp_talk:A>gw1>B>gw2')
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
from .scenario import span

HERE = os.path.dirname(os.path.abspath(__file__))


def lab(args, opts):
    """inside the namespaces: one fleet, start to finish"""
    sc = importlib.import_module(".scenarios." + args.scenario, __package__)
    impl = load(args.impl, opts)
    fleet = sc.FLEETS[args.fleet[0]]
    out = os.path.join(args.out, args.fleet[0])
    lb = Lab(sc.TOPOLOGY, fleet, out)
    try:
        for g in lb.gw.values():
            if g.policy["offload"] and not impl.offload:
                g.policy["offload"] = False
            impl.install(g)
        for g in lb.gw.values():
            impl.start(g)
        flows = [dict(f) for f in sc.FLOWS if args.flow in f["id"]]
        lb.place(flows)
        for n, f in enumerate(flows):
            f["start"] = 0.01 * n               # not all in the same instant
        time.sleep(2)                           # the implementations settle
        cpu0 = {n: g.cpu_ms() for n, g in lb.gw.items()}
        job = dict(t0=time.monotonic() + 3, grace=expect.GRACE)
        agents, parts = [], []
        for role, ends in (("server", lb.servers), ("client", lb.clients)):
            for e in ends.values():
                mine = [f for f in flows if f[role] == e.name]
                jf = os.path.join(out, "%s.job.json" % e.node.name)
                rf = os.path.join(out, "%s.jsonl" % e.node.name)
                with open(jf, "w") as fh:
                    json.dump(dict(job, flows=mine), fh)
                agents.append(e.node.spawn(sys.executable, os.path.join(HERE, "agent.py"), role, jf, rf,
                                           log=os.path.join(out, "%s.log" % e.node.name)))
                parts.append(rf)
        for a in agents:
            a.wait()
        seen = {"client": {}, "server": {}}
        for rf in parts:
            for line in open(rf):
                r = json.loads(line)
                seen[r["role"]][r["id"]] = r
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
        bad, retry = expect.judge(f, c, s)
        res.append(dict(flow=f, client=c, server=s, bad=bad, retry=retry))
    with open(os.path.join(out, "results.json"), "w") as fh:
        json.dump(dict(scenario=args.scenario, impl=args.impl, fleet=args.fleet[0], dir=out,
                       gateways=gws, flows=res), fh, indent=1)
    return 0


def main():
    ap = argparse.ArgumentParser(prog="gwlab", usage=__doc__)
    ap.add_argument("scenario")
    ap.add_argument("--impl", required=True)
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--fleet", nargs="*", default=[])
    ap.add_argument("--flow", default="")
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
    longest = max(span(f["p"]) for f in sc.FLOWS if args.flow in f["id"])
    print("%s on %s: fleets %s, about %d s; results in %s"
          % (args.scenario, args.impl, ", ".join(fleets), longest + 20, args.out), flush=True)
    wrap = ["unshare", "-n"] if root else ["unshare", "-Urn"]
    if not root and subprocess.run(["systemd-run", "--user", "--scope", "-q", "true"],
                                   capture_output=True).returncode == 0:
        wrap = ["systemd-run", "--user", "--scope", "-q", "-p", "Delegate=yes"] + wrap
    procs = []
    for fl in fleets:
        os.makedirs(os.path.join(args.out, fl), exist_ok=True)
        argv = wrap + [sys.executable, "-m", "gwlab", args.scenario, "--inside", "--impl", args.impl,
                       "--fleet", fl, "--flow", args.flow, "--out", args.out]
        for kv in args.set:
            argv += ["--set", kv]
        log = open(os.path.join(args.out, fl, "lab.log"), "w")
        procs.append((fl, subprocess.Popen(argv, cwd=os.path.dirname(HERE), stdout=log, stderr=log)))
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
