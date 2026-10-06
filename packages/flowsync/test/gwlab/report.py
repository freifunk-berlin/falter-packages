"""The two tables of a lab run, and the failures."""
import statistics


def table(rows, head):
    w = [max(len(str(r[i])) for r in [head] + rows) for i in range(len(head))]
    out = ["  ".join(str(c).ljust(w[i]) for i, c in enumerate(head)).rstrip(),
           "  ".join("-" * n for n in w)]
    out += ["  ".join(str(c).ljust(w[i]) for i, c in enumerate(r)).rstrip() for r in rows]
    return "\n".join(out)


def connects(res):
    c = res["client"] or {}
    return [r["connect_ms"] for r in c.get("conns", [c]) if r.get("connect_ms") is not None]


def render(run):
    """run: what a lab wrote to results.json"""
    out = ["== %s, %s, fleet %s: %d flows, %d FAIL =="
           % (run["scenario"], run["impl"], run["fleet"], len(run["flows"]),
              sum(1 for r in run["flows"] if r["bad"]))]
    groups = {}
    for r in run["flows"]:
        f = r["flow"]
        key = (f["traffic"], "asym" if f["asym"] else "sym",
               "bypass" if run["gateways"][f["rev"]]["policy"]["bypass"] else "strict")
        groups.setdefault(key, []).append(r)
    rows = []
    for key, rs in sorted(groups.items()):
        cl = [r["client"] or {} for r in rs]
        sv = [r["server"] or {} for r in rs]
        cms = [x for r in rs for x in connects(r)]
        rows.append(list(key) + [
            len(rs), sum(1 for r in rs if not r["bad"]), sum(1 for r in rs if r["bad"]),
            sum(1 for r in rs if r["retry"]),
            sum(s.get("lost_start", 0) + s.get("lost_late", 0) for s in sv),
            sum(c.get("lost_start", 0) + c.get("lost_late", 0) + c.get("answered", []).count(False)
                for c in cl),
            max([c.get("outage_ms", 0) for c in cl] + [0]),
            "%d / %d" % (statistics.median(cms), max(cms)) if cms else "-",
        ])
    out.append(table(rows, ["traffic", "path", "return gw", "flows", "pass", "FAIL", "needed retry",
                            "lost c>s", "lost s>c", "worst gap ms", "connect ms p50 / max"]))
    out.append("")
    rows = []
    for name, g in run["gateways"].items():
        cpu = g["cpu_ms"]
        rows.append([name, "bypass" if g["policy"]["bypass"] else "strict",
                     "yes" if g["policy"]["offload"] else "no",
                     "%.0f / %.0f" % tuple(cpu) if cpu else "n/a",
                     g["sync_tx"][0], "%.0f" % (g["sync_tx"][1] / 1000)])
    out.append(table(rows, ["gateway", "rules", "offload", "cpu ms user / sys", "sync pkts", "sync kB"]))
    bad = [r for r in run["flows"] if r["bad"]]
    for r in bad[:12]:
        out.append("FAIL %s (path RTT %d ms): %s" % (r["flow"]["id"], r["flow"]["rtt_ms"],
                                                     "; ".join(r["bad"])))
    if len(bad) > 12:
        out.append("... and %d more, see %s" % (len(bad) - 12, run["dir"]))
    return "\n".join(out)
