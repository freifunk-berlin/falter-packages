"""Predicates shared by the scenarios."""


def native_unreplied(e):
    return e.is_native and not e.seen_reply


def gone(g, f):
    return not g.ct(f).alive


def all_gone(gs, f):
    return lambda: all(gone(g, f) for g in gs)


def long_lived(e):
    """a copy that saw traffic: ASSURED with a timeout well above a refresh's,
    or in the flowtable"""
    return e.alive and e.assured and (e.offloaded or e.timeout > 99)


def tcp_state(e, state):
    """TCP state, or offloaded (the dump shows no state for those)"""
    return e.alive and (e.tcp_state == state or (e.offloaded and e.tcp_state is None))


EST, SYN_SENT, CLOSE = 3, 1, 8
