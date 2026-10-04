"""Gateway profiles, combination specs and the named matrices.

A combination is a list of per-gateway profiles. Spec strings:

  g3                 three gateways (default 3)
  plain | offload    flow offloading on none / all gateways
  mixed              offloading on all gateways but the last
  offload=0,1        offloading on the listed gateways
  ack | noack        the stateless TCP ACK/RST accept on all / no gateways (default: all)
  ack=0,2            ... on the listed gateways
  g1:offload,noack   per-gateway override (offload | plain | ack | noack)

e.g. "g3 mixed ack", "g2 plain noack", "g3 plain g0:offload g2:noack".
"""
import itertools


class Profile:
    __slots__ = ("offload", "ack")

    def __init__(self, offload=False, ack=True):
        self.offload = offload
        self.ack = ack

    def __eq__(self, o):
        return (self.offload, self.ack) == (o.offload, o.ack)

    def __repr__(self):
        return "%s%s" % ("O" if self.offload else "-", "A" if self.ack else "-")


class Combo:
    def __init__(self, profiles):
        self.p = list(profiles)

    @property
    def n(self):
        return len(self.p)

    @property
    def name(self):
        off = "".join(str(i) for i, p in enumerate(self.p) if p.offload) or "-"
        ack = "".join(str(i) for i, p in enumerate(self.p) if p.ack) or "-"
        return "g%d off=%s ack=%s" % (self.n, off, ack)

    def spec(self):
        """a spec string that parses back to this combination"""
        parts = ["g%d" % self.n, "plain", "noack"]
        for i, p in enumerate(self.p):
            opts = (["offload"] if p.offload else []) + (["ack"] if p.ack else [])
            if opts:
                parts.append("g%d:%s" % (i, ",".join(opts)))
        return " ".join(parts)

    def __eq__(self, o):
        return self.p == o.p

    def __repr__(self):
        return "Combo(%s)" % self.name


def _indexes(v, n):
    """"0,2", or as in a combination's name: "02", "-" for none"""
    out = set()
    if v == "-":
        return out
    for x in (v.split(",") if "," in v else list(v)):
        x = x.strip()
        if not x:
            continue
        i = int(x)
        if not 0 <= i < n:
            raise ValueError("gateway index %d out of range (g%d)" % (i, n))
        out.add(i)
    return out


def parse(spec):
    toks = spec.split()
    n = 3
    for t in toks:
        if t[0] == "g" and t[1:].isdigit():
            n = int(t[1:])
    if not 1 <= n <= 8:
        raise ValueError("gateway count %d out of range" % n)
    prof = [Profile() for _ in range(n)]
    for t in toks:
        if t[0] == "g" and t[1:].isdigit():
            continue
        if t == "plain":
            for p in prof:
                p.offload = False
        elif t in ("offload", "offload-all"):
            for p in prof:
                p.offload = True
        elif t == "mixed":
            for i, p in enumerate(prof):
                p.offload = i < n - 1
        elif t.startswith(("offload=", "off=")):
            idx = _indexes(t.partition("=")[2], n)
            for i, p in enumerate(prof):
                p.offload = i in idx
        elif t == "ack":
            for p in prof:
                p.ack = True
        elif t == "noack":
            for p in prof:
                p.ack = False
        elif t.startswith("ack="):
            idx = _indexes(t[4:], n)
            for i, p in enumerate(prof):
                p.ack = i in idx
        elif t[0] == "g" and ":" in t:
            i = int(t[1:t.index(":")])
            if not 0 <= i < n:
                raise ValueError("gateway index %d out of range (g%d)" % (i, n))
            for o in t[t.index(":") + 1:].split(","):
                if o == "offload":
                    prof[i].offload = True
                elif o == "plain":
                    prof[i].offload = False
                elif o == "ack":
                    prof[i].ack = True
                elif o == "noack":
                    prof[i].ack = False
                else:
                    raise ValueError("unknown gateway option %r" % o)
        else:
            raise ValueError("unknown spec token %r" % t)
    return Combo(prof)


MATRICES = {
    # production-like first; then offloading everywhere and mixed, then the
    # pure stateful firewall, and two gateways
    "default": ["g3 plain ack", "g3 offload ack", "g3 mixed ack", "g3 plain noack",
                "g3 mixed noack", "g2 plain ack"],
    "quick": ["g3 plain ack"],
}


def full():
    out = []
    for n in (2, 3, 4):
        offs = [set(), set(range(n)), set(range(n - 1)), {0}]
        acks = [set(range(n)), set(), set(range(n - 1))]
        for off, ack in itertools.product(offs, acks):
            c = Combo([Profile(i in off, i in ack) for i in range(n)])
            if c not in out:
                out.append(c)
    return out


def expand(matrix=None, profiles=()):
    """the combinations of a run: explicit profiles, or a named matrix"""
    if profiles:
        return [parse(s) for s in profiles]
    if matrix == "full":
        return full()
    if matrix not in MATRICES:
        raise ValueError("unknown matrix %r (%s, full)" % (matrix, ", ".join(MATRICES)))
    return [parse(s) for s in MATRICES[matrix]]
