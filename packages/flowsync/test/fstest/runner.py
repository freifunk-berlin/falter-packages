"""Running scenarios.

The parent expands the matrix into runs, one per (scenario, combination),
and spreads each combination's runs over a few workers, longest first
(durations of earlier runs are kept in ~/.cache). A worker is a child process
in a network namespace of its own (unshare -n, as root: BPF programs cannot
be loaded from a user namespace), so workers never see each other; it builds its topology once and resets it between scenarios,
which keeps namespace churn (serialized in the kernel) low. "heavy" scenarios
(floods of thousands of flows or records a second) take one of a few slots so
that not too many run at the same time.
"""
import fcntl
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback

from . import matrix
from .check import Checks, Log, Skip
from .env import Env
from .scenario import load, select

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))     # test/
DURATIONS = os.path.expanduser("~/.cache/flowsync-fstest-durations.json")


def paths(args):
    return {
        "flowsync": os.path.abspath(args.flowsync),
        "bpf_object": os.path.abspath(args.bpf_object),
        "probe": os.path.join(HERE, "probe.py"),
        "ptyrun": os.path.join(HERE, "ptyrun.py"),
        "python": sys.executable,
    }


def heavy_slot(lockdir, slots):
    """an exclusive lock on one of `slots` lock files; returns the open file"""
    while True:
        for i in range(slots):
            f = open(os.path.join(lockdir, "heavy%d.lock" % i), "a")
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return f
            except BlockingIOError:
                f.close()
        time.sleep(0.2)



# ------------------------------------------------------------------ child
def child(args):
    """a worker (inside unshare -n): run a list of scenarios in one
    combination on one topology, reset between scenarios; after a failure the
    topology is rebuilt, so a broken state never leaks into the next one"""
    combo = matrix.parse(args.profile)
    reg = load()
    topo = None
    for name in args.scenarios:
        sc = reg[name]
        wd = os.path.join(args.out, name)
        os.makedirs(wd, exist_ok=True)
        log = Log(args.log, prefix="[%s %s] " % (combo.name, name), echo=False,
                  own=os.path.join(wd, "checks.log"))
        lock = heavy_slot(args.lockdir, args.heavy) if "heavy" in sc.tags and args.lockdir else None
        checks = Checks(log)
        log.line("=== %s ===" % name)
        t0 = time.monotonic()
        env = None
        res, reason = "PASS", ""
        try:
            env = Env(combo, paths(args), wd, checks, topo=topo)
            topo = env.topo
            sc.fn(env)
        except Skip as e:
            res, reason = "SKIP", str(e)
        except Exception:
            checks.fail("exception: " + traceback.format_exc().strip().replace("\n", " | "))
        # a daemon the scenario did not stop must still be running (ptyrun
        # passes on its exit status: 128 + n for signal n)
        if env and res != "SKIP":
            for g in env.g:
                if g.proc is not None and g.proc.poll() is not None:
                    checks.fail("%s: flowsync exited during the scenario (status %s)"
                                % (g, g.proc.returncode))
        if lock:
            lock.close()
        if res != "SKIP" and checks.fails:
            res = "FAIL"
        keep = res != "FAIL"
        try:
            if env:
                env.close(keep=keep)
        except Exception:
            checks.fail("teardown: " + traceback.format_exc().strip().splitlines()[-1])
            res, keep = "FAIL", False
            try:
                topo.close()
            except Exception:
                pass
        if not keep:
            topo = None
        secs = time.monotonic() - t0
        log.line("--- %s: %s%s (%.0fs)" % (name, res, " (%d)" % checks.fails if checks.fails else "",
                                          secs))
        r = {"scenario": name, "result": res, "fails": checks.fails, "oks": checks.oks,
             "secs": round(secs, 1), "reason": reason, "dir": wd}
        print("RESULT " + json.dumps(r), flush=True)
    if topo:
        topo.close()
    return 0


# ----------------------------------------------------------------- parent
def _load_durations():
    try:
        with open(DURATIONS) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def _save_durations(d):
    try:
        os.makedirs(os.path.dirname(DURATIONS), exist_ok=True)
        with open(DURATIONS, "w") as f:
            json.dump(d, f, indent=0, sort_keys=True)
    except OSError:
        pass


def parent(args):
    combos = matrix.expand(args.matrix, args.profile_list)
    scs = select(args.scenarios, args.tags)
    names = [s.name for s in scs]
    root = args.out or tempfile.mkdtemp(prefix="flowsync-test.")
    os.makedirs(root, exist_ok=True)
    if args.log:
        open(args.log, "w").close()
    log = Log(args.log, echo=True)

    table = {}          # scenario -> combination index -> result
    units = []
    seen = {}           # (scenario, what it sees of a combination) -> combination index
    for k, c in enumerate(combos):
        for sc in scs:
            why = sc.skip_reason(c, k == 0)
            same = k if why else seen.setdefault((sc.name, sc.key(c)), k)
            if why:
                table.setdefault(sc.name, {})[k] = {"result": "SKIP", "reason": why}
            elif same != k:
                table.setdefault(sc.name, {})[k] = {
                    "result": "SAME", "reason": "same as c%d for it (depends on %s only)"
                    % (same, ", ".join(sorted(sc.depends)))}
            else:
                units.append((k, c, sc))
    dur = _load_durations()

    def est(u):
        return dur.get("%s|%s" % (u[2].name, u[1].name), dur.get(u[2].name, 30))

    # workers: the heavy runs of a combination go to one worker of their own,
    # one after the other (their stalls would fail timing checks elsewhere);
    # the others are split, longest first, over a share of the jobs
    # proportional to their total duration
    by_combo = {}
    workers = []
    for u in units:
        by_combo.setdefault(u[0], []).append(u)
    for k in sorted(by_combo):
        heavy = [u for u in by_combo[k] if "heavy" in u[2].tags]
        by_combo[k] = [u for u in by_combo[k] if "heavy" not in u[2].tags]
        if heavy:
            workers.append((sum(est(u) for u in heavy), k, combos[k], [u[2] for u in heavy],
                            "h"))
    # workers per combination: one each, then every further one to the
    # combination with the most work per worker, which keeps the longest
    # worker as short as the split allows (rounding a proportional share
    # left a small combination on one worker that then finished last)
    load = {k: sum(est(u) for u in us) for k, us in by_combo.items() if us}
    nw = {k: 1 for k in load}
    for _ in range(max(0, args.jobs - len(nw))):
        k = max((k for k in nw if nw[k] < len(by_combo[k])),
                key=lambda k: load[k] / nw[k], default=None)
        if k is None:
            break
        nw[k] += 1
    for k, us in sorted(by_combo.items()):
        if not us:
            continue
        n = nw[k]
        bins = [[0, []] for _ in range(n)]
        for u in sorted(us, key=lambda u: -est(u)):
            b = min(bins, key=lambda b: b[0])
            b[0] += est(u)
            b[1].append(u)
        for w, (load_s, bus) in enumerate(bins):
            if bus:
                workers.append((load_s, k, combos[k], [u[2] for u in bus], w))
    workers.sort(key=lambda w: -w[0])

    log.line("flowsync tests: %d scenarios x %d combinations = %d runs on %d workers "
             "(heavy: %d at a time); logs in %s"
             % (len(names), len(combos), len(units), len(workers), args.heavy, root))
    for k, c in enumerate(combos):
        log.line("  c%d = %s" % (k, c.name))

    lock = threading.Lock()
    t_start = time.monotonic()
    done = [0]

    def report(k, combo, r):
        with lock:
            table.setdefault(r["scenario"], {})[k] = r
            done[0] += 1
            if r["result"] != "SKIP" and r.get("secs"):
                dur["%s|%s" % (r["scenario"], combo.name)] = r["secs"]
            if getattr(args, "quiet", False) and r["result"] == "PASS":
                return
            print("%s [%3d/%d] %-22s %s %s%s" % (
                time.strftime("%H:%M:%S"), done[0], len(units), combo.name, r["result"],
                r["scenario"],
                " (%s)" % r["reason"] if r.get("reason") and r["result"] != "PASS" else ""),
                flush=True)

    def run_worker(load_s, k, combo, scl, w):
        out = os.path.join(root, "c%d" % k)
        # a network namespace only: loading BPF programs needs the
        # capabilities of the initial user namespace
        argv = ["unshare", "-n", sys.executable, "-m", "fstest", "--child",
                "--profile", combo.spec(), "--out", out, "--flowsync", args.flowsync,
                "--bpf-object", args.bpf_object, "--scenarios", *[s.name for s in scl],
                "--lockdir", root, "--heavy", str(args.heavy)]
        if args.log:
            argv += ["--log", args.log]
        env = dict(os.environ, PYTHONPATH=HERE + os.pathsep + os.environ.get("PYTHONPATH", ""))
        os.makedirs(out, exist_ok=True)
        seen = set()
        with open(os.path.join(out, "worker%s.out" % w), "w") as cf:
            p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=cf, text=True, env=env,
                                 cwd=HERE)
            for line in p.stdout:
                if not line.startswith("RESULT "):
                    cf.write(line)
                    continue
                r = json.loads(line[7:])
                seen.add(r["scenario"])
                report(k, combo, r)
            p.wait()
        for sc in scl:
            if sc.name not in seen:
                report(k, combo, {"scenario": sc.name, "result": "FAIL", "fails": 1, "secs": 0,
                                  "reason": "worker exited %d before it, see %s/worker%s.out"
                                  % (p.returncode, out, w), "dir": out})

    threads = [threading.Thread(target=run_worker, args=w) for w in workers]
    for t in threads:
        t.start()
        time.sleep(0.3)     # not all topologies built in the same instant
    for t in threads:
        t.join()
    _save_durations(dur)

    # the table: one row per scenario, one column per combination
    bad = 0
    w = max([len(n) for n in names] + [8])
    log.line("")
    log.line("%-*s %s" % (w, "", "  ".join("c%d" % k for k in range(len(combos)))))
    for n in names:
        row = []
        for k in range(len(combos)):
            e = table.get(n, {}).get(k)
            row.append({"PASS": "ok", "FAIL": "XX", "SKIP": "--", "SAME": "=="}.get(e["result"], "??")
                       if e else "??")
            bad += bool(e and e["result"] == "FAIL")
        log.line("%-*s %s" % (w, n, "  ".join(row)))
    for k, c in enumerate(combos):
        log.line("c%d = %s" % (k, c.name))
    log.line("ok passed, XX failed, -- skipped (-v: why), == same as an earlier column "
             "in what the scenario depends on")
    if args.verbose:
        skips = {}
        for n in names:
            for k, e in table.get(n, {}).items():
                if e["result"] in ("SKIP", "SAME"):
                    skips.setdefault((n, e.get("reason", "")), []).append("c%d" % k)
        if skips:
            log.line("skipped or same:")
            for (n, why), ks in sorted(skips.items()):
                log.line("  %s (%s): %s" % (n, why, " ".join(ks)))
    for n in names:
        for k, e in sorted(table.get(n, {}).items()):
            if e["result"] == "FAIL":
                log.line("FAILED %s in %s: %s" % (n, combos[k].name, e.get("dir", "")))
    log.line("%s: %d failures in %.0f s%s"
             % ("FAIL" if bad else "PASS", bad, time.monotonic() - t_start,
                "; logs and status files in %s" % root if bad or args.keep else ""))
    with open(os.path.join(root, "results.json"), "w") as f:
        json.dump({"combos": [c.name for c in combos], "results": table}, f, indent=1)
    if not bad and not args.keep:
        shutil.rmtree(root, ignore_errors=True)     # keep the logs of failed runs only
    return 1 if bad else 0
