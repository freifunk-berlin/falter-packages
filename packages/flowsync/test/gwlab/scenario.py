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


def span(p):
    """seconds a flow of this traffic lasts"""
    return p["seconds"] if "seconds" in p else p.get("connections", p.get("requests")) * p["every"]
