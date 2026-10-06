"""The two tables of a lab run, and the failures."""
import statistics

from .expect import sustained


def table(rows, head):
    w = [max(len(str(r[i])) for r in [head] + rows) for i in range(len(head))]
    out = ["  ".join(str(c).ljust(w[i]) for i, c in enumerate(head)).rstrip(),
           "  ".join("-" * n for n in w)]
    out += ["  ".join(str(c).ljust(w[i]) for i, c in enumerate(r)).rstrip() for r in rows]
    return "\n".join(out)


def connects(res):
    c = res["client"] or {}
    return [r["connect_ms"] for r in c.get("conns", [c]) if r.get("connect_ms") is not None]


def measured(r):
    """a measurement's result in words"""
    f, c, s = r["flow"], r["client"] or {}, r["server"] or {}
    kind = f["p"]["kind"]
    if kind == "udp_flood":
        return "%d kpps" % ((s if f["p"]["dir"] == "up" else c).get("delivered_pps", 0) / 1000)
    if kind == "tcp_bulk":
        return "%s Mbit/s, connect %d ms" % (c.get("mbit"), c.get("connect_ms") or 0)
    steps = c.get("steps", [])
    return "%d flows/s sustained (%s)" % (
        sustained(steps), ", ".join("%d: %d%%" % (st["achieved"], 100 * st["answered"] / max(1, st["sent"]))
                                    for st in steps))


def per_packet(r):
    """a flood's cost in the kernel of the gateway that forwarded it"""
    f, p = r["flow"], r["flow"]["p"]
    if p["kind"] != "udp_flood":
        return "-"
    up = p["dir"] == "up"
    pps = ((r["server"] if up else r["client"]) or {}).get("delivered_pps", 0)
    k = ((r["cpu_ms"] or {}).get(f["fwd"] if up else f["rev"]) or {}).get("kernel")
    return "%d" % (k * 1e6 / (pps * p["seconds"])) if pps and k is not None else "n/a"


def render_alone(run):
    out = ["== %s, %s, fleet %s ==" % (run["scenario"], run["impl"], run["fleet"])]
    rows = []
    for r in run["flows"]:
        f, cpu = r["flow"], r["cpu_ms"] or {}
        others = [n for n in cpu if n not in (f["fwd"], f["rev"])]

        def show(n):
            """user / system time of the implementation's processes + the kernel's packet path"""
            w = cpu.get(n) or {}
            return "%s + %s" % ("%d / %d" % tuple(w["cpu"]) if w.get("cpu") else "n/a",
                                w["kernel"] if w.get("kernel") is not None else "n/a")
        rows.append([f["traffic"], "asym" if f["asym"] else "sym",
                     "FAIL: " + "; ".join(r["bad"]) if r["bad"] else measured(r), per_packet(r),
                     show(f["fwd"]), show(f["rev"]) if f["asym"] else "-",
                     show(others[0]) if others else "-"])
    out.append(table(rows, ["traffic", "path", "result", "kernel ns/packet",
                            "ms user / sys + kernel: fwd gw", "return gw", "idle peer"]))
    return "\n".join(out)


def render(run):
    """run: what a lab wrote to results.json"""
    if run.get("alone"):
        return render_alone(run)
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
            sum(len(s.get("lost", [])) for s in sv),
            sum(len(c.get("lost", [])) + c.get("answered", []).count(False) for c in cl),
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
