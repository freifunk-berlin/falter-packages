"""The scenario registry.

  @scenario(gateways=3, tags={"slow"})
  def name(env): ...

gateways: how many of the lab's gateways the scenario uses (the lab has
three). tags: for selection; "slow" is left out unless named, "heavy" runs
alone, one at a time.
"""

REGISTRY = {}


class Scenario:
    def __init__(self, fn, gateways, tags):
        self.fn = fn
        self.name = fn.__name__
        self.gateways = gateways
        self.tags = set(tags)
        self.doc = (fn.__doc__ or "").strip()


def scenario(gateways=2, tags=()):
    def reg(fn):
        if fn.__name__ in REGISTRY:
            raise ValueError("duplicate scenario %s" % fn.__name__)
        REGISTRY[fn.__name__] = Scenario(fn, gateways, tags)
        return fn
    return reg


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
    if tags:
        return [s for s in reg.values() if s.tags & set(tags)]
    return [s for s in reg.values() if "slow" not in s.tags]
