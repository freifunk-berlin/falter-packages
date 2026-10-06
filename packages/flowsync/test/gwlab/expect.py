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
blackout, an uplink re-created) cannot be won while it lasts and until the
sync has delivered again: from its start until its end + the sync latency
(and one path RTT around it, for what is on its way).

Nothing ever excuses a reset or a stalled connection. If answers never passed
in the measurement, nothing is accepted on an asymmetric path.

judge() returns (what is wrong: a list, empty = pass; whether the flow needed
a retry: reported, not judged).
"""
SYN_RETRIES = (1, 3, 7, 15)     # seconds after the first SYN at which Linux sends it again
JITTER_MS = 300                 # timing noise of the lab in a connect time; well below a SYN retry


def judge(f, c, s, events=(), sync_ms=None):
    """f: the flow, c / s: what the client's and the server's agent saw,
    events: the scenario's ("at": seconds after the first flow's start),
    sync_ms: the measured sync latency, None if answers never passed"""
    if c is None or s is None or "crashed" in c or "crashed" in s:
        return ["no result from the %s" % ("client" if c is None or "crashed" in (c or {})
                                           else "server")], False
    sync = (sync_ms or 0) / 1000
    lost_start = max(0, sync - f["margin_ms"] / 1000) if f["asym"] and sync_ms is not None else 0
    # an event also hits what is on its way: one path RTT before and after
    rtt = f["rtt_ms"] / 1000
    win = [(e["at"] - f["start"] - rtt, e["at"] + e.get("seconds", 0) + sync + rtt - f["start"])
           for e in events]

    def excused(t):
        """may what the client sends t seconds into the flow go unanswered?
        None, or why ("start", "event") and until when"""
        for a, b in win:
            if a <= t < b:
                return "event", b
        return ("start", lost_start) if t < lost_start else None
    return RULES[f["p"]["kind"]](f, c, s, excused)


def udp_rr(f, c, s, excused):
    lost = [k for k, ok in enumerate(c["answered"]) if not ok]
    bad = [k for k in lost if not excused(k * f["p"]["every"])]
    return ((["request %s unanswered" % ", ".join(map(str, bad))] if bad else []),
            any(excused(k * f["p"]["every"]) for k in lost))


def udp_stream(f, c, s, excused):
    p, bad = f["p"], []
    up = [k for k in s["lost"] if not excused(k / p["up_pps"])]
    down = [k for k in c["lost"] if not excused(k / p["down_pps"])]
    if up:
        bad.append("client>server lost %d, first at %.1f s" % (len(up), up[0] / p["up_pps"]))
    if down:
        bad.append("server>client lost %d, first at %.2f s, longest gap %d ms"
                   % (len(down), down[0] / p["down_pps"], c["outage_ms"]))
    return bad, any(excused(k / p["down_pps"]) for k in c["lost"])


def connect_ok(f, r, t, excuse, what):
    """a connection started t seconds into the flow came up: at once, or, if
    the race cannot be won until some time, by the first SYN retransmission
    after that time"""
    if r["connect_ms"] is None:
        return "%s: %s" % (what, r.get("error", "no connection"))
    retry = next((x for x in SYN_RETRIES if excuse and x >= excuse[1] - t), 0 if not excuse else 31)
    if r["connect_ms"] > f["rtt_ms"] + 1000 * retry + JITTER_MS:
        return "%s: connect took %d ms (path RTT %d ms, %s)" % (
            what, r["connect_ms"], f["rtt_ms"],
            "SYN retry expected after %d s" % retry if retry else "no retry expected")
    return None


def slow(f, r):
    return bool(r["connect_ms"] and r["connect_ms"] > f["rtt_ms"] + 700)


def tcp_short(f, c, s, excused):
    bad, retry = [], False
    for k, r in enumerate(c["conns"]):
        # every connection is a new flow with its own start: the event
        # windows are in flow time, the start window is in connection time
        t = k * f["p"]["every"]
        ev, st = excused(t), excused(0)
        excuse = ev if ev and ev[0] == "event" else (("start", t + st[1]) if st and st[0] == "start" else None)
        e = connect_ok(f, r, t, excuse, "connection %d" % k)
        if not e and not r["ok"]:
            e = "connection %d: %s" % (k, r.get("error", "failed"))
        if e:
            bad.append(e)
        retry |= slow(f, r)
    return bad, retry


def tcp_talk(f, c, s, excused):
    e = connect_ok(f, c, 0, excused(0), "connect")
    if not e and not c["done"]:
        e = "broke after %s s, %d exchanges: %s" % (c.get("failed_at_s"), c["exchanges"],
                                                    c.get("error"))
    return ([e] if e else []), slow(f, c)


def udp_ladder(f, c, s, excused):
    """a number, not a verdict, with one limit: an answer a second late must
    pass, or even a TCP client's retry would not"""
    if not f["asym"]:
        return (["answers lost on a symmetric path"] if not all(c["answered"]) else []), False
    return ([] if c["answered"][-1] else
            ["the answer %d ms late did not pass" % f["p"]["delays_ms"][-1]]), False


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
def udp_flood(f, c, s, excused):
    pps = (s if f["p"]["dir"] == "up" else c).get("delivered_pps", 0)
    return ([] if pps else ["nothing delivered"]), False


def tcp_bulk(f, c, s, excused):
    return ([] if c["done"] and c["mbit"] else ["no transfer: %s" % c.get("error")]), False


def udp_newflows(f, c, s, excused):
    return ([] if any(st["answered"] for st in c["steps"]) else ["no flow was answered"]), False


def sustained(steps):
    """the highest rate of new flows that was sent as asked and answered to 99 %"""
    ok = [st["achieved"] for st in steps
          if st["answered"] >= 0.99 * st["sent"] and st["achieved"] >= 0.9 * st["rate"]]
    return max(ok) if ok else 0


RULES = dict(udp_rr=udp_rr, udp_stream=udp_stream, tcp_short=tcp_short, tcp_talk=tcp_talk,
             udp_flood=udp_flood, tcp_bulk=tcp_bulk, udp_newflows=udp_newflows, udp_ladder=udp_ladder)
