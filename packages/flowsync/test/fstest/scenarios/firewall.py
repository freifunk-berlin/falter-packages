"""Real sockets through the gateways' forward chains (fw4-like: policy drop,
established accept, reject)."""
import os

from ..scenario import need, scenario
from .common import SYN_SENT, all_gone, native_unreplied

MEGA = 1048576


def xchg(env, f, timeout=3):
    return env.probe(env.cl, "xchg", f.c, f.cport, f.s, f.sport, timeout, check=False)


@scenario(gateways=2, tags={"firewall"})
def fw_udp(env):
    """The client sends through g0, the server replies through g1. Without a
    copy g1's forward chain rejects the reply; with the copy from g0's NEW
    event it passes, and g1's copy turns ASSURED from forwarding it."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1, real=True)
    env.spawn(env.sv, "echo", f.s, f.sport, 1, 4)
    env.sleep(0.3)
    g1.stop()
    r0 = g1.fwc("rej")
    env.check("no daemon on g1: the reply is rejected by g1's forward chain", xchg(env, f),
              "^NONE$")
    env.check("g1 counted the reject", g1.fwc("rej") - r0, 1)
    env.check("g0 native: unreplied, unmarked", g0.ct(f), native_unreplied)
    env.check("g1 has no entry", g1.ct(f).alive, False)
    g1.start()
    env.wait_for("g1's daemon up", 8, g1.up)
    f2 = env.flow("udp", fw=g0, rev=g1, real=True)
    env.spawn(env.sv, "echo", f2.s, f2.sport, 1, 4)
    env.sleep(0.3)
    env.check("with g1 synced, the reply of a new flow passes", xchg(env, f2, 5), "^RECV$")
    g1.block_sync(g0)
    env.check("a second exchange on the flow passes as well", xchg(env, f2, 5), "^RECV$")
    env.check("g1 copy carries traffic from forwarding the replies", g1.ct(f2),
              lambda e: e.is_copy and e.carries_traffic(env.E))
    env.check("g0 native still unreplied: no reply came through g0", g0.ct(f2), native_unreplied)
    env.check("g1 rejected nothing else", g1.fwc("rej") - r0, 1)
    env.wait_st("g1 announced its copy from the forwarded replies", g1, "tx_copies", 1)
    g1.unblock_sync()
    env.wait_for("everything gone after the exchange",
                 env.udp_life(g1, True) + env.E + 3 * env.I, all_gone(env.g, f2))


@scenario(gateways=3, tags={"firewall", "tcp"})
def fw_tcp(env):
    """Real TCP stacks over the asymmetric path, 1 MiB each way: the SYN and the
    client's data through g0, the SYN/ACK and the server's data through g1,
    where only the injected ESTABLISHED copy lets them pass."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("tcp", fw=g0, rev=g1, real=True)
    out = os.path.join(env.dir, "srv.out")
    cout = os.path.join(env.dir, "cli.out")
    srv = env.spawn(env.sv, "tcpsrv", f.s, f.sport, MEGA, out=out)
    env.sleep(0.5)
    # the SYN/ACK must not beat the copy to g1 (with the ACK rule it would
    # pass and leave a reversed entry): g1 holds the server's packets until
    # the copy is there, the server retransmits
    g1.hold(f)
    cli = env.spawn(env.cl, "tcpcli", f.c, f.cport, f.s, f.sport, MEGA, 20, out=cout)
    env.wait_for("g1 has the copy from g0's NEW event", 3, lambda: g1.ct(f).is_copy, step=0.05)
    g1.release()
    # while the connection runs: afterwards an offloaded copy may already be
    # torn down, closed and gone
    env.wait_for("g1's copy turns ASSURED from the server's segments", 20,
                 lambda: g1.ct(f).is_copy and g1.ct(f).assured, step=0.1)
    cli.wait()
    srv.wait()
    env.check("connection and 1 MiB each way completed [OK connect_ms total_ms]",
              open(cout).read().strip(), "^OK")
    env.check("the server saw the whole upload", open(out).read().strip(), "^SERVED %d$" % MEGA)
    env.check("g0 native SYN_SENT and unmarked: it never saw the SYN/ACK", g0.ct(f),
              lambda e: e.is_native and e.tcp_state == SYN_SENT)
    env.check("g0 saw the client's segments as invalid", g0.fwc("inv"), lambda n: n >= 1)
    env.check("and forwarded them anyway", g0.fwc("rej"), 0)
    env.wait_for("g0's native and g2's copy gone after the connection closed", 45,
                 lambda: not g0.ct(f).alive and not g2.ct(f).alive)
    env.check("g1's copy closes on its own timeout (at most 120 s)", g1.ct(f),
              lambda e: not e.alive or (e.timeout <= 120 and (e.tcp_state or 0) >= 4))


@scenario(gateways=3, requires=need(lambda p: p[1].offload, "needs flow offloading on g1"),
          tags={"offload"})
def offload(env):
    """A copy carrying a rerouted flow in both directions is offloaded: its
    packets bypass conntrack and the dump shows no timeout. Being offloaded is
    the evidence: g1 announces the copy every round and g2 keeps its copy;
    once the traffic stops everything goes."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1, real=True)
    env.spawn(env.sv, "echo", f.s, f.sport, 0.05, 60)
    env.sleep(0.3)
    # the echo answers within 50 ms (for offloading to start quickly): let the
    # copy arrive on g1 before the first exchange
    env.probe(env.cl, "send", f.c, f.cport, f.s, f.sport)
    env.wait_for("g1 has the copy", 3, lambda: g1.ct(f).is_copy)
    env.check("first exchange: out via g0, reply via g1 on the copy", xchg(env, f, 5), "^RECV$")
    f.reroute(fw=g1)            # g1 carries both directions now
    lp = env.loop(25, 1, lambda: xchg(env, f, 2))
    env.wait_for("g1's copy is offloaded", 6, lambda: g1.ct(f).offloaded)
    env.ok("g1's copy while offloaded: %s" % g1.ct(f))
    c0 = g1.st("tx_copies") or 0
    env.hold("g2 keeps its copy while g1 carries the flow offloaded", 18,
             lambda: g2.ct(f).alive, step=2)
    env.check("g1 announced its offloaded copy every round", (g1.st("tx_copies") or 0) - c0,
              lambda n: n >= 4)
    env.check("g1 reports it offloaded", g1.st("copies_offloaded"), lambda n: n >= 1)
    lp.wait()
    env.wait_for("everything gone after the traffic stopped",
                 g1.ft_idle + env.timers["udp_stream"] + env.E + 3 * env.I,
                 all_gone(env.g[:3], f))


@scenario(gateways=3, requires=need(lambda p: p[0].offload, "needs flow offloading on g0"),
          tags={"offload"})
def offload_native(env):
    """A symmetric flow through g0, offloaded from its second packet on:
    conntrack never sees the packet that would set ASSURED, so the native stays
    SEEN_REPLY without ASSURED. It is announced from its own dump phase, and the
    other gateways keep their copies while it carries traffic."""
    g0, g1, g2 = env.g[:3]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0, real=True)
    env.spawn(env.sv, "echo", f.s, f.sport, 0.05, 60)
    env.sleep(0.3)
    lp = env.loop(25, 1, lambda: xchg(env, f, 2))
    env.wait_for("g0's native is offloaded", 6, lambda: g0.ct(f).offloaded)
    env.check("g0's native: offloaded, replied, not ASSURED, unmarked", g0.ct(f),
              lambda e: e.is_native and e.seen_reply and not e.assured and e.offloaded)
    env.hold("g2 keeps a copy of the offloaded native", 18, lambda: g2.ct(f).alive, step=2)
    env.check("g1 has a copy too", g1.ct(f), lambda e: e.is_copy)
    lp.wait()
    env.wait_for("everything gone after the traffic stopped",
                 g0.ft_idle + env.timers["udp_stream"] + env.E + 3 * env.I,
                 all_gone(env.g[:3], f))
