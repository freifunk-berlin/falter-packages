"""Implementations: what makes a gateway forward a flow it did not see start.

An implementation is one class in one file. The lab gives it a gateway (a
namespace with mesh0 towards the clients and wan0 towards the Internet, the
peers' sync addresses, a policy and a directory) and never looks inside:

  install(gw)     everything the gateway needs before traffic: its firewall
                  for gw.policy (bypass: the stateless accept for TCP segments
                  with ACK or RST; offload: flow offloading, if supported),
                  kernel settings, configuration
  start(gw)       start syncing (processes via gw.spawn, so their CPU is counted)
  stop(gw)
  lose_state(gw)  forget every flow, as a flush or a reboot would

Options come from the command line (--set key=value), e.g. the binary to test.
"""
import importlib
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TEST = os.path.dirname(os.path.dirname(HERE))       # packages/flowsync/test

NAMES = ("none", "flowsync", "conntrackd", "flowsync_bpf")


class Impl:
    needs_root = False          # cannot run in an unprivileged user namespace
    offload = True              # can do flow offloading

    def __init__(self, opts):
        self.opts = opts

    def install(self, gw):
        raise NotImplementedError

    def start(self, gw):
        pass

    def stop(self, gw):
        pass

    def lose_state(self, gw):
        raise NotImplementedError

    def broken(self, timers):
        """reasons why this implementation's own timers no longer relate to
        the others as in production at this scale (see Timers.broken)"""
        return []


def load(name, opts):
    if name not in NAMES:
        raise SystemExit("unknown implementation %r (%s)" % (name, ", ".join(NAMES)))
    return importlib.import_module("." + name, __name__).IMPL(opts)


def children(pid):
    """pids whose parent is pid"""
    out = []
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                with open("/proc/%s/stat" % d) as f:
                    st = f.read()
                if int(st[st.rindex(")") + 2:].split()[1]) == pid:
                    out.append(int(d))
            except (OSError, ValueError):
                pass
    return out
