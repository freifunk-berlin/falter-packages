"""What "works" means for a flow. No implementation appears in this file,
and no fixed allowance for loss: what is accepted follows from how fast the
implementation syncs in this lab (measured before the flows, from outside)
and from the protocols' own retry timers.

One gateway for both directions (symmetric): clean from the first packet.

Two gateways (asymmetric): the return gateway learns the flow from the forward
one. The sync and the server's answer travel the same uplinks; the answer
additionally goes to the server and back (the path's margin). So:

  sync latency <= margin   the race can be won: clean from the first packet
  sync latency >  margin   it cannot: what the client sends in the first
                           (sync latency - margin) of a flow gets no answer.
                           That much loss is accepted, and the endpoint's own
                           retry has to repair it: the first UDP request after
                           that time is answered, the first TCP SYN
                           retransmission after it connects.

An event of the scenario (a reroute, a gateway losing its state, a sync
blackout, an uplink re-created): how fast an implementation repairs what the
event broke is its own business and a number in the report (recovery: from
the end of the event to the last packet lost), not a verdict. What is judged:
the flow delivers again before it ends, and loss before the event follows the
rules above.

Nothing ever excuses a reset or a stalled connection. If answers never passed
in the measurement, nothing is accepted on an asymmetric path.

judge() returns (what is wrong: a list, empty = pass; whether the flow needed
a retry at its start: reported, not judged; recovery in seconds after an
event, None if the event cost the flow nothing).
"""
SYN_RETRIES = (1, 3, 7, 15)     # seconds after the first SYN at which Linux sends it again
JITTER_MS = 300                 # timing noise of the lab in a connect time; well below a SYN retry


class Path:
    """what a flow's path and the scenario's events make of a moment t
    (seconds into the flow)"""

    def __init__(self, f, events, sync_ms):
        sync = (sync_ms or 0) / 1000
        self.rtt = f["rtt_ms"] / 1000
        # what the client sends this long into the flow cannot be answered
        self.lost_start = (max(0, sync - f["margin_ms"] / 1000)
                           if f["asym"] and sync_ms is not None else 0)
        # (start, end) of each event in flow time; it also hits what is on its way
        self.events = sorted((e["at"] - f["start"] - self.rtt, e["at"] + e.get("seconds", 0) - f["start"])
                             for e in events)

    def start(self, t):
        return t < self.lost_start

    def event(self, t):
        """the end of the last event that began before t, or None"""
        ends = [b for a, b in self.events if a <= t]
        return ends[-1] if ends else None


def after_event(x, lost_at, last_at):
    """losses at the times lost_at, the flow's last packet at last_at: returns
    (losses no event explains, did it recover, recovery in seconds or None)"""
    unexplained = [t for t in lost_at if x.event(t) is None and not x.start(t)]
    hit = [t for t in lost_at if x.event(t) is not None]
    if not hit:
        return unexplained, True, None
    return unexplained, last_at not in hit, max(0.0, max(t - x.event(t) for t in hit))


def judge(f, c, s, events=(), sync_ms=None):
    """f: the flow, c / s: what the client's and the server's agent saw,
    events: the scenario's ("at": seconds after the first flow's start),
    sync_ms: the measured sync latency, None if answers never passed"""
    if c is None or s is None or "crashed" in c or "crashed" in s:
        return ["no result from the %s" % ("client" if c is None or "crashed" in (c or {})
                                           else "server")], False, None
    return RULES[f["p"]["kind"]](f, c, s, Path(f, events, sync_ms))


def udp_rr(f, c, s, x):
    every, n = f["p"]["every"], len(c["answered"])
    lost = [k * every for k, ok in enumerate(c["answered"]) if not ok]
    bad, recovered, recovery = after_event(x, lost, (n - 1) * every)
    out = ["request at %s s unanswered" % ", ".join("%g" % t for t in bad)] if bad else []
    if not recovered:
        out.append("no answers again after the event")
    return out, any(x.start(t) for t in lost), recovery


def udp_stream(f, c, s, x):
    p, out, rec = f["p"], [], []
    for name, lost, pps in (("client>server", s["lost"], p["up_pps"]),
                            ("server>client", c["lost"], p["down_pps"])):
        at = [k / pps for k in lost]
        bad, recovered, recovery = after_event(x, at, (int(pps * p["seconds"]) - 1) / pps)
        if bad:
            out.append("%s lost %d, first at %.2f s" % (name, len(bad), bad[0]))
        if not recovered:
            out.append("%s did not deliver again after the event" % name)
        rec.append(recovery)
    rec = [r for r in rec if r is not None]
    return out, any(x.start(k / p["down_pps"]) for k in c["lost"]), max(rec) if rec else None


def connect_ok(f, r, t, x, what, t0=0):
    """a connection started t seconds into the flow came up: at once; or, if
    the race cannot be won at its start, by the first SYN retransmission
    after it can; or, under an event, at all"""
    if r["connect_ms"] is None:
        return "%s: %s" % (what, r.get("error", "no connection"))
    if x.event(t) is not None:
        return None
    retry = next((n for n in SYN_RETRIES if n >= x.lost_start), 31) if x.lost_start else 0
    if r["connect_ms"] > f["rtt_ms"] + 1000 * retry + JITTER_MS:
        return "%s: connect took %d ms (path RTT %d ms, %s)" % (
            what, r["connect_ms"], f["rtt_ms"],
            "SYN retry expected after %d s" % retry if retry else "no retry expected")
    return None


def slow(f, r):
    return bool(r["connect_ms"] and r["connect_ms"] > f["rtt_ms"] + 700)


def tcp_short(f, c, s, x):
    bad, retry = [], False
    for k, r in enumerate(c["conns"]):          # every connection is a new flow with its own start
        e = connect_ok(f, r, k * f["p"]["every"], x, "connection %d" % k)
        if not e and not r["ok"]:
            e = "connection %d: %s" % (k, r.get("error", "failed"))
        if e:
            bad.append(e)
        retry |= slow(f, r)
    return bad, retry, None


def tcp_talk(f, c, s, x):
    e = connect_ok(f, c, 0, x, "connect")
    if not e and not c["done"]:
        e = "broke after %s s, %d exchanges: %s" % (c.get("failed_at_s"), c["exchanges"],
                                                    c.get("error"))
    return ([e] if e else []), slow(f, c), None


def tcp_idle(f, c, s, x):
    """silence is no reason to lose a connection: one gateway keeps it for
    the established timeout, and so must two"""
    e = connect_ok(f, c, 0, x, "connect")
    if not e and not c["done"]:
        e = "got as far as '%s', then after %s s: %s" % (c["stage"], c.get("failed_at_s"), c.get("error"))
    return ([e] if e else []), slow(f, c), None


def udp_ladder(f, c, s, x):
    """a number, not a verdict, with one limit: an answer a second late must
    pass, or even a TCP client's retry would not"""
    if not f["asym"]:
        return (["answers lost on a symmetric path"] if not all(c["answered"]) else []), False, None
    return ([] if c["answered"][-1] else
            ["the answer %d ms late did not pass" % f["p"]["delays_ms"][-1]]), False, None


def sync_latency(flows):
    """from the ladders of the asymmetric flows: per delay how many answers
    passed, and the shortest delay from which all did"""
    rs = [r for r in flows if r["flow"]["p"]["kind"] == "udp_ladder" and r["flow"]["asym"] and r["client"]
          and "answered" in r["client"]]
    if not rs:
        return None
    delays = rs[0]["flow"]["p"]["delays_ms"]
    passed = [sum(r["client"]["answered"][k] for r in rs) for k in range(len(delays))]
    ok = [d for k, d in enumerate(delays) if all(n == len(rs) for n in passed[k:])]
    return dict(flows=len(rs), delays_ms=delays, passed=passed, all_from_ms=ok[0] if ok else None)


# Measurements produce numbers, not verdicts; they fail only if nothing got through.
def udp_flood(f, c, s, x):
    pps = (s if f["p"]["dir"] == "up" else c).get("delivered_pps", 0)
    return ([] if pps else ["nothing delivered"]), False, None


def tcp_bulk(f, c, s, x):
    return ([] if c["done"] and c["mbit"] else ["no transfer: %s" % c.get("error")]), False, None


def udp_newflows(f, c, s, x):
    return ([] if any(st["answered"] for st in c["steps"]) else ["no flow was answered"]), False, None


def sustained(steps):
    """the highest rate of new flows that was sent as asked and answered to 99 %"""
    ok = [st["achieved"] for st in steps
          if st["answered"] >= 0.99 * st["sent"] and st["achieved"] >= 0.9 * st["rate"]]
    return max(ok) if ok else 0


RULES = dict(udp_rr=udp_rr, udp_stream=udp_stream, tcp_short=tcp_short, tcp_talk=tcp_talk,
             udp_flood=udp_flood, tcp_bulk=tcp_bulk, udp_newflows=udp_newflows, udp_ladder=udp_ladder, tcp_idle=tcp_idle)
