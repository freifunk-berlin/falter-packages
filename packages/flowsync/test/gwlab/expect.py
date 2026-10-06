"""What "works" means for a flow. No implementation appears in this file.

One gateway for both directions (symmetric): clean from the first packet.

Two gateways (asymmetric): the return gateway has to learn the flow from the
forward one, and the server's first answer may be faster than that. So the
start of a flow (of every connection, for TCP) may cost one attempt, which the
endpoint's own retry repairs: the next UDP request is answered, a TCP client's
SYN retransmission connects. After GRACE seconds the flow has to be clean.

An event of the scenario (a reroute, a gateway losing its state, a sync
blackout, an uplink re-created) may cost packets from its start until
EVENT_GRACE seconds after its end, on any flow.

Nothing may ever reset or stall a connection.

judge() returns (what is wrong: a list, empty = pass; whether the flow needed
a retry at its start: reported, not judged).
"""
GRACE = 1.5         # seconds: one TCP SYN retransmission (1 s) and some air
EVENT_GRACE = 5     # seconds after an event; the lab's timers are about 10 x shorter than production's


def judge(f, c, s, events=()):
    """f: the flow, c / s: what the client's and the server's agent saw,
    events: the scenario's, with "at" in seconds after the first flow's start"""
    if c is None or s is None or "crashed" in c or "crashed" in s:
        return ["no result from the %s" % ("client" if c is None or "crashed" in (c or {})
                                           else "server")], False
    win = [(e["at"] - f["start"], e["at"] + e.get("seconds", 0) + EVENT_GRACE - f["start"])
           for e in events]

    def excused(t):
        """may something go wrong t seconds into the flow? None, or why
        ("start", "event") and until when"""
        for a, b in win:
            if a <= t < b:
                return "event", b
        return ("start", GRACE) if f["asym"] and t < GRACE else None
    return RULES[f["p"]["kind"]](f, c, s, excused)


def udp_rr(f, c, s, excused):
    lost = [k for k, ok in enumerate(c["answered"]) if not ok]
    bad = [k for k in lost if not excused(k * f["p"]["every"])]
    return ((["request %s unanswered" % ", ".join(map(str, bad))] if bad else []),
            any((excused(k * f["p"]["every"]) or [0])[0] == "start" for k in lost))


def udp_stream(f, c, s, excused):
    p, bad = f["p"], []
    up = [k for k in s["lost"] if not excused(k / p["up_pps"])]
    down = [k for k in c["lost"] if not excused(k / p["down_pps"])]
    if up:
        bad.append("client>server lost %d, first at %.1f s" % (len(up), up[0] / p["up_pps"]))
    if down:
        bad.append("server>client lost %d, first at %.1f s, longest gap %d ms"
                   % (len(down), down[0] / p["down_pps"], c["outage_ms"]))
    return bad, any((excused(k / p["down_pps"]) or [0])[0] == "start" for k in c["lost"])


def connect_ok(f, r, t, excuse, what):
    """a connection started t seconds into the flow came up: at once, or
    (excused) by a SYN retransmission: the one after 1 s at the start of a
    flow, the first one after an event's grace is over"""
    if r["connect_ms"] is None:
        return "%s: %s" % (what, r.get("error", "no connection"))
    limit = 700
    if excuse:
        limit = 1000 * (GRACE if excuse[0] == "start" else excuse[1] - t + 4) + 300
    if r["connect_ms"] > f["rtt_ms"] + limit:
        return "%s: connect took %d ms (path RTT %d ms)" % (what, r["connect_ms"], f["rtt_ms"])
    return None


def slow(f, r):
    return bool(r["connect_ms"] and r["connect_ms"] > f["rtt_ms"] + 700)


def tcp_short(f, c, s, excused):
    bad, retry = [], False
    for k, r in enumerate(c["conns"]):
        # every connection is a new flow: its own start
        t = k * f["p"]["every"]
        excuse = excused(t) or (("start", GRACE) if f["asym"] else None)
        e = connect_ok(f, r, t, excuse, "connection %d" % k)
        if not e and not r["ok"]:
            e = "connection %d: %s" % (k, r.get("error", "failed"))
        if e:
            bad.append(e)
        retry |= slow(f, r) and bool(excuse)
    return bad, retry


def tcp_talk(f, c, s, excused):
    e = connect_ok(f, c, 0, excused(0), "connect")
    if not e and not c["done"]:
        e = "broke after %s s, %d exchanges: %s" % (c.get("failed_at_s"), c["exchanges"],
                                                    c.get("error"))
    return ([e] if e else []), slow(f, c)


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
             udp_flood=udp_flood, tcp_bulk=tcp_bulk, udp_newflows=udp_newflows)
