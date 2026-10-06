"""Combinations a scenario runs in: how many gateways.

  g2 | g3      two or three gateways (default 3)

Scenarios say how many they need (`gateways=`); one that needs fewer than a
combination has runs once, in the first combination that fits.
"""


class Combo:
    def __init__(self, n):
        self.n = n
        self.p = [None] * n     # per gateway; nothing differs between them here

    @property
    def name(self):
        return "g%d" % self.n

    def spec(self):
        return self.name

    def __eq__(self, o):
        return self.n == o.n

    def __repr__(self):
        return "Combo(%s)" % self.name


def parse(spec):
    toks = spec.split()
    n = 3
    for t in toks:
        if t[0] == "g" and t[1:].isdigit():
            n = int(t[1:])
        else:
            raise ValueError("unknown spec token %r" % t)
    if not 2 <= n <= 8:
        raise ValueError("gateway count %d out of range" % n)
    return Combo(n)


MATRICES = {
    "default": ["g3"],
    "quick": ["g3"],
}


def expand(matrix=None, profiles=()):
    """the combinations of a run: explicit ones, or a named matrix"""
    if profiles:
        return [parse(s) for s in profiles]
    if matrix not in MATRICES:
        raise ValueError("unknown matrix %r (%s)" % (matrix, ", ".join(MATRICES)))
    return [parse(s) for s in MATRICES[matrix]]
