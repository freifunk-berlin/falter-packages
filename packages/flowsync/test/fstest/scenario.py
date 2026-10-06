"""The scenario registry.

  @scenario(gateways=3, requires=need(lambda p: p[1].ack, "g1 has the ACK rule"), tags={"tcp"})
  def name(env): ...

gateways: the minimum gateway count; requires: called with the list of
profiles, returns None to run or a reason to skip; once: profile-independent,
runs in the first combination of a run only; tags: for selection ("slow" is
left out unless named, and so is "repro": a reproducer of a known defect
that fails until its fix lands; "heavy" runs alone, one at a time); depends: the
gateway properties the scenario's outcome depends on (default: the ACK rule
for scenarios tagged "tcp", the only traffic it applies to, nothing else). A combination equal to an earlier one in those
properties is not run again for the scenario.
"""

REGISTRY = {}


class Scenario:
    def __init__(self, fn, gateways, requires, tags, once, depends):
        self.fn = fn
        self.name = fn.__name__
        self.gateways = gateways
        self.requires = requires
        self.tags = set(tags)
        self.once = once
        self.depends = set(depends if depends is not None else
                           {"ack"} if "tcp" in self.tags else set())
        self.doc = (fn.__doc__ or "").strip()

    def skip_reason(self, combo, first):
        if combo.n < self.gateways:
            return "needs %d gateways" % self.gateways
        if self.once and not first:
            return "profile-independent, runs in the first combination"
        if self.requires:
            return self.requires(combo.p)
        return None

    def key(self, combo):
        """what the scenario sees of a combination"""
        return (combo.n, tuple((p.offload if "offload" in self.depends else None,
                                p.ack if "ack" in self.depends else None) for p in combo.p))


def scenario(gateways=2, requires=None, tags=(), once=False, depends=None):
    def reg(fn):
        if fn.__name__ in REGISTRY:
            raise ValueError("duplicate scenario %s" % fn.__name__)
        REGISTRY[fn.__name__] = Scenario(fn, gateways, requires, tags, once, depends)
        return fn
    return reg


def need(pred, reason):
    """a requirement: pred(profiles) must hold, else skip with reason"""
    return lambda p: None if pred(p) else reason


def load():
    from . import scenarios  # noqa: F401  (registers everything)
    return REGISTRY


def select(names=(), tags=()):
    reg = load()
    names = list(dict.fromkeys(names))     # once each, in the given order
    if names:
        missing = [n for n in names if n not in reg]
        if missing:
            raise ValueError("unknown scenario(s): %s" % ", ".join(missing))
        return [reg[n] for n in names]
    out = [s for s in reg.values() if not s.tags & {"slow", "repro"}]
    if tags:
        out = [s for s in reg.values() if s.tags & set(tags)]
    return out
