"""Building the flow list of a scenario."""
import itertools

from .scenarios.traffic import TRAFFIC


def grid(traffic, client, server, fwd, rev):
    """every combination: one flow per (traffic, client, server, forward
    gateway, return gateway). A flow is named traffic:client>fwd>server>rev."""
    flows = []
    for t, c, s, f, r in itertools.product(traffic, client, server, fwd, rev):
        flows.append(dict(id="%s:%s>%s>%s>%s" % (t, c, f, s, r), traffic=t, p=TRAFFIC[t],
                          client=c, server=s, fwd=f, rev=r, cport=40000, sport=443))
    return flows


SCALED = ("tcp_short", "tcp_talk", "tcp_idle", "udp_rr", "udp_stream")      # traffic written in production time


def in_lab(p, timers):
    """a traffic description in lab time"""
    if p["kind"] not in SCALED:
        return p
    p = dict(p)
    for k in ("seconds", "every", "idle"):
        if k in p:
            p[k] = timers.span(p[k])
    for k in ("up_pps", "down_pps"):
        if k in p:
            p[k] = p[k] * timers.scale
    return p


def span(p):
    """seconds a flow of this traffic lasts"""
    if p["kind"] == "udp_newflows":
        return len(p["rates"]) * (p["step_seconds"] + p["reply_delay"] + 1.2) + 1
    if p["kind"] == "tcp_idle":
        return 2 * p["idle"] + 4
    if p["kind"] == "udp_ladder":
        return sum(p["delays_ms"]) / 1000 + len(p["delays_ms"]) * 0.5
    if p["kind"] == "udp_flood":
        return p["seconds"] + 3
    return p["seconds"] if "seconds" in p else p.get("connections", p.get("requests")) * p["every"]
