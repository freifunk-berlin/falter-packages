"""What "works" means for a flow. No implementation appears in this file.

One gateway for both directions (symmetric): clean from the first packet.

Two gateways (asymmetric): the return gateway has to learn the flow from the
forward one, and the server's first answer may be faster than that. So the
start of a flow (of every connection, for TCP) may cost one attempt, which the
endpoint's own retry repairs: the next UDP request is answered, a TCP client's
SYN retransmission connects. After GRACE seconds the flow has to be clean,
and nothing may ever reset a connection.

judge() returns (what is wrong: a list, empty = pass; whether the flow needed
a retry: reported, not judged).
"""
GRACE = 1.5         # seconds: one TCP SYN retransmission (1 s) and some air


def judge(f, c, s):
    """f: the flow, c / s: what the client's and the server's agent saw"""
    if c is None or s is None or "crashed" in c or "crashed" in s:
        return ["no result from the %s" % ("client" if c is None or "crashed" in (c or {})
                                           else "server")], False
    return RULES[f["p"]["kind"]](f, c, s, GRACE if f["asym"] else 0)


def udp_rr(f, c, s, grace):
    ans = c["answered"]
    first_lost = bool(grace) and not ans[0]
    bad = [k for k, ok in enumerate(ans) if not ok and not (grace and k == 0)]
    return (["request %s unanswered" % ", ".join(map(str, bad))] if bad else []), first_lost


def udp_stream(f, c, s, grace):
    bad = []
    if s["lost_start"] + s["lost_late"]:
        bad.append("client>server lost %d" % (s["lost_start"] + s["lost_late"]))
    if c["lost_late"]:
        bad.append("server>client lost %d after the start, longest gap %d ms"
                   % (c["lost_late"], c["outage_ms"]))
    if c["lost_start"] and not grace:
        bad.append("server>client lost %d at the start" % c["lost_start"])
    return bad, bool(grace) and c["lost_start"] > 0


def connect_ok(f, r, grace, what):
    """a connection came up: at once, or (grace) after one SYN retransmission"""
    limit = f["rtt_ms"] + 1000 * grace + 300
    if r["connect_ms"] is None:
        return "%s: %s" % (what, r.get("error", "no connection"))
    if r["connect_ms"] > (limit if grace else f["rtt_ms"] + 700):
        return "%s: connect took %d ms (path RTT %d ms)" % (what, r["connect_ms"], f["rtt_ms"])
    return None


def tcp_short(f, c, s, grace):
    bad, retry = [], False
    for k, r in enumerate(c["conns"]):
        e = connect_ok(f, r, grace, "connection %d" % k)
        if not e and not r["ok"]:
            e = "connection %d: %s" % (k, r.get("error", "failed"))
        if e:
            bad.append(e)
        retry |= bool(r["connect_ms"] and r["connect_ms"] > f["rtt_ms"] + 700)
    return bad, retry


def tcp_talk(f, c, s, grace):
    e = connect_ok(f, c, grace, "connect")
    if not e and not c["done"]:
        e = "broke after %s s, %d exchanges: %s" % (c.get("failed_at_s"), c["exchanges"],
                                                    c.get("error"))
    return ([e] if e else []), bool(c["connect_ms"] and c["connect_ms"] > f["rtt_ms"] + 700)


RULES = dict(udp_rr=udp_rr, udp_stream=udp_stream, tcp_short=tcp_short, tcp_talk=tcp_talk)
