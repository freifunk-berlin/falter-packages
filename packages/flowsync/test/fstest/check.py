"""Checks and the timestamped progress log.

Every check writes one line ("ok" or "FAIL") to the progress log and counts
failures; scenarios never raise on a failed check, so one run reports all of
them. Waits log a "..." line when they start, so `tail -f` shows what a run is
waiting for.
"""
import re
import sys
import time


class Skip(Exception):
    """raised by a scenario (or its requirements) that cannot run here"""


class Log:
    def __init__(self, path, prefix="", echo=True, own=None):
        self.f = open(path, "a", buffering=1) if path else None
        self.own = open(own, "a", buffering=1) if own else None
        self.prefix = prefix
        self.echo = echo

    def line(self, text, echo=None):
        t = time.strftime("%H:%M:%S")
        if self.f:
            self.f.write("%s %s%s\n" % (t, self.prefix, text))
        if self.own:
            self.own.write("%s %s\n" % (t, text))
        s = "%s %s%s" % (t, self.prefix, text)
        if self.echo if echo is None else echo:
            print(s, flush=True)


class Checks:
    def __init__(self, log):
        self.log = log
        self.fails = 0
        self.oks = 0

    def ok(self, text):
        self.oks += 1
        self.log.line("  ok   " + text)

    def fail(self, text):
        self.fails += 1
        self.log.line("  FAIL " + text)

    def note(self, text):
        self.log.line("  ...  " + text, echo=False)

    def check(self, desc, value, want):
        """want: a regex (matched against str(value)), a callable, or a plain value"""
        if callable(want):
            good = bool(want(value))
            shown = "predicate"
        elif isinstance(want, str):
            good = re.search(want, str(value)) is not None
            shown = "/%s/" % want
        else:
            good = value == want
            shown = repr(want)
        if good:
            self.ok("%s [%s]" % (desc, value))
        else:
            self.fail("%s: got [%s], want %s" % (desc, value, shown))
        return good

    def true(self, desc, cond, detail=""):
        if cond:
            self.ok(desc + (" [%s]" % detail if detail != "" else ""))
        else:
            self.fail(desc + (": [%s]" % detail if detail != "" else ""))
        return cond

    def wait_for(self, desc, timeout, fn, step=0.25):
        t0 = time.monotonic()
        self.note("%s (up to %ss)" % (desc, timeout))
        while True:
            try:
                if fn():
                    self.ok("%s (after %.0fs)" % (desc, time.monotonic() - t0))
                    return True
            except Exception as e:      # a query failing counts as "not yet"
                last = e
            if time.monotonic() - t0 >= timeout:
                self.fail("%s (not within %ss)" % (desc, timeout))
                return False
            time.sleep(step)

    def hold(self, desc, secs, fn, step=1.0):
        t0 = time.monotonic()
        self.note("%s (for %ss)" % (desc, secs))
        while time.monotonic() - t0 < secs:
            if not fn():
                self.fail("%s broke at +%.0fs" % (desc, time.monotonic() - t0))
                return False
            time.sleep(step)
        self.ok("%s held for %ss" % (desc, secs))
        return True


def sleep(secs):
    time.sleep(secs)


def eprint(*a):
    print(*a, file=sys.stderr, flush=True)
