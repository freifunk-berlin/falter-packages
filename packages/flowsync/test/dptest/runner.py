"""Running scenarios.

The parent spreads the scenarios over a few workers, longest first
(durations of earlier runs are kept in ~/.cache). A worker is a child process
in a network namespace of its own (unshare -n, as root: BPF programs cannot
be loaded from a user namespace), so workers never see each other; it builds
its topology once and resets it between scenarios, which keeps namespace
churn (serialized in the kernel) low. "heavy" scenarios (floods of thousands
of flows or records a second) run in a worker of their own, one after the
other.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback

from .check import Checks, Log, Skip
from .env import Env
from .scenario import load, select

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))     # test/
GATEWAYS = 3            # the lab; a scenario uses two or all of them
DURATIONS = os.path.expanduser("~/.cache/flowsync-dptest-durations.json")


def paths(args):
    return {
        "flowsync": os.path.abspath(args.flowsync),
        "bpf_object": os.path.abspath(args.bpf_object),
        "probe": os.path.join(HERE, "dptest", "probe.py"),
        "ptyrun": os.path.join(HERE, "ptyrun.py"),
        "python": sys.executable,
    }


# ------------------------------------------------------------------ child
def child(args):
    """a worker (inside unshare -n): run a list of scenarios on one topology,
    reset between scenarios; after a failure the topology is rebuilt, so a
    broken state never leaks into the next one"""
    reg = load()
    topo = None
    for name in args.scenarios:
        sc = reg[name]
        wd = os.path.join(args.out, name)
        os.makedirs(wd, exist_ok=True)
        log = Log(args.log, prefix="[%s] " % name, echo=False,
                  own=os.path.join(wd, "checks.log"))
        checks = Checks(log)
        log.line("=== %s ===" % name)
        t0 = time.monotonic()
        env = None
        res, reason = "PASS", ""
        try:
            env = Env(GATEWAYS, paths(args), wd, checks, topo=topo)
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
    scs = select(args.scenarios, args.tags)
    names = [s.name for s in scs]
    root = args.out or tempfile.mkdtemp(prefix="flowsync-dptest.")
    own_root = not args.out                 # made here: removed after an all-pass run
    os.makedirs(root, exist_ok=True)
    if args.log:
        open(args.log, "w").close()
    log = Log(args.log, echo=True)
    dur = _load_durations()

    def est(sc):
        return dur.get(sc.name, 30)

    # workers: the heavy scenarios go to one worker of their own, one after
    # the other (their stalls would fail timing checks elsewhere); the others
    # are split, longest first, over the remaining jobs
    workers = []
    heavy = [s for s in scs if "heavy" in s.tags]
    rest = [s for s in scs if "heavy" not in s.tags]
    if heavy:
        workers.append((heavy, "h"))
    bins = [[0, []] for _ in range(max(1, min(args.jobs, len(rest))))]
    for sc in sorted(rest, key=lambda s: -est(s)):
        b = min(bins, key=lambda b: b[0])
        b[0] += est(sc)
        b[1].append(sc)
    workers += [(b[1], w) for w, b in enumerate(bins) if b[1]]

    log.line("flowsync dptest: %d scenarios on %d workers; logs in %s"
             % (len(names), len(workers), root))

    lock = threading.Lock()
    t_start = time.monotonic()
    table = {}          # scenario -> result
    done = [0]

    def report(r):
        with lock:
            table[r["scenario"]] = r
            done[0] += 1
            if r.get("secs"):
                dur[r["scenario"]] = r["secs"]
            if getattr(args, "quiet", False) and r["result"] == "PASS":
                return
            print("%s [%3d/%d] %s %s%s" % (
                time.strftime("%H:%M:%S"), done[0], len(names), r["result"], r["scenario"],
                " (%s)" % r["reason"] if r.get("reason") and r["result"] != "PASS" else ""),
                flush=True)

    def run_worker(scl, w):
        # a network namespace only: loading BPF programs needs the
        # capabilities of the initial user namespace
        argv = ["unshare", "-n", sys.executable, "-m", "dptest", "--child",
                "--out", root, "--flowsync", args.flowsync,
                "--bpf-object", args.bpf_object, "--scenarios", *[s.name for s in scl]]
        if args.log:
            argv += ["--log", args.log]
        env = dict(os.environ, PYTHONPATH=HERE + os.pathsep + os.environ.get("PYTHONPATH", ""))
        seen = set()
        with open(os.path.join(root, "worker%s.out" % w), "w") as cf:
            p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=cf, text=True, env=env,
                                 cwd=HERE)
            for line in p.stdout:
                if not line.startswith("RESULT "):
                    cf.write(line)
                    continue
                r = json.loads(line[7:])
                seen.add(r["scenario"])
                report(r)
            p.wait()
        for sc in scl:
            if sc.name not in seen:
                report({"scenario": sc.name, "result": "FAIL", "fails": 1, "secs": 0,
                        "reason": "worker exited %d before it, see %s/worker%s.out"
                        % (p.returncode, root, w), "dir": root})

    threads = [threading.Thread(target=run_worker, args=w) for w in workers]
    for t in threads:
        t.start()
        time.sleep(0.3)     # not all topologies built in the same instant
    for t in threads:
        t.join()
    _save_durations(dur)

    bad = [n for n in names if table.get(n, {}).get("result") != "PASS"]
    w = max([len(n) for n in names] + [8])
    log.line("")
    for n in names:
        e = table.get(n)
        log.line("%-*s %s" % (w, n, {"PASS": "ok", "FAIL": "FAIL", "SKIP": "skipped"}.get(
            e["result"], "??") if e else "??"))
    for n in bad:
        log.line("FAILED %s: %s" % (n, table.get(n, {}).get("dir", "")))
    log.line("%s: %d of %d failed in %.0f s%s"
             % ("FAIL" if bad else "PASS", len(bad), len(names), time.monotonic() - t_start,
                "; logs and status files in %s" % root if bad or args.keep or not own_root
                else ""))
    with open(os.path.join(root, "results.json"), "w") as f:
        json.dump(table, f, indent=1)
    if not bad and not args.keep and own_root:
        shutil.rmtree(root, ignore_errors=True)     # keep the logs of failed runs only
    return 1 if bad else 0
