"""Operations as seen inside a gateway: the datapath without its daemon, a
reboot, the uplink device created anew, rules and programs that disappear,
the first start, limits, privileges. What a fleet's flows make of such events
end to end is gwlab's."""
import re

from ..gateway import PORT, SERVER_NET, children
from ..scenario import scenario
from .common import hook_names, rejects, replies, synced, tcx


@scenario(gateways=2)
def daemon_down(env):
    """All daemons stop. New flows on a symmetric path still work (the
    programs stay on the uplink), an asymmetric flow lives until its copy
    expires; the alive elements expire, so what leaves is tracked again.
    After the restart the first round brings the peer's flow back at once."""
    g0, g1 = env.g[:2]
    env.start()
    fa = env.flow("udp", fw=g0, rev=g1)
    fa.send()
    synced(env, fa)
    for g in env.g:
        g.stop()
    fs = env.flow("udp", fw=g0, rev=g0)
    fs.send()
    env.check("daemons down: a new symmetric flow passes (of 3)", replies(env, fs), 3)
    env.check("the asymmetric one still passes on g1's copy (of 3)", replies(env, fa), 3)
    env.wait_for("the alive elements expire within alive_timeout", 12,
                 lambda: not any(g.alive() for g in env.g), step=0.5)
    lp = env.loop(100, 1, fa.send)
    env.sleep(env.E + 2)
    env.check("after an element_timeout without announcements: rejected (of 2)",
              replies(env, fa, 2), 0)
    env.start()
    env.wait_for("restarted: g1 holds the flow again", 2, lambda: g1.ft(fa).remote, step=0.05)
    env.check("replies pass g1 again (of 3)", replies(env, fa), 3)
    env.wait_for("the alive elements are back", 5, lambda: all(g.alive() for g in env.g), step=0.2)
    lp.stop()


@scenario(gateways=2)
def restart_keeps(env):
    """One daemon restarts: its maps are reused, so neither its own flows nor
    the peers' are interrupted, and the programs are replaced in place."""
    g0, g1 = env.g[:2]
    env.start()
    fa = env.flow("udp", fw=g0, rev=g1)
    fb = env.flow("udp", fw=g1, rev=g1)
    lp = env.loop(100, 0.5, lambda: (fa.send(), fb.send()))
    synced(env, fa)
    g1.restart()
    d = replies(env, fa, 10, gap=0.1) + replies(env, fb, 10, gap=0.1)
    env.check("replies during g1's restart, both flows (of 20)", d, 20)
    env.wait_for("g1 is up again", 5, g1.up)
    env.check("g1 kept the peer's flow and its own", [g1.ft(fa).remote, g1.ft(fb).local],
              [True, True])
    env.check("nothing rejected", rejects(env), 0)
    env.wait_st("g0 served g1's resync request", g0, "rx_resync", 1)
    lp.stop()


@scenario(gateways=2)
def reboot(env):
    """g1 comes up empty (no maps, no programs, no rules of ours): it asks
    for a resync, and g0's flows are back well before g0's next regular
    round."""
    g0, g1 = env.g[:2]
    env.start()
    fl = [env.flow("udp", fw=g0, rev=g1) for _ in range(5)]
    lp = env.loop(100, 1, lambda: [f.send() for f in fl])
    synced(env, fl[-1])
    g0.tick()               # g0's next regular round is a whole interval away
    g1.reset()
    env.check("g1 is empty", g1.cmd("flows"), "^$")
    g1.start()
    env.wait_for("g1 holds all five again", 1.5, lambda: all(g1.ft(f).remote for f in fl),
                 step=0.05)
    env.check("replies pass g1 (of 3)", replies(env, fl[0]), 3)
    env.wait_st("g0 served the request", g0, "rx_resync", 1)
    lp.stop()


def recreate_wan(env, g):
    """the uplink device goes and comes back under its name with a new index,
    as netifd does to a VLAN on ifup"""
    i = g.i
    g.node.sh("ip link del wan0")
    g.node.sh("ip link add wan0 type veth peer name w%d netns %d && ip link set wan0 up && "
              "ip -6 addr add fd00:%d:2::1/64 dev wan0 nodad && "
              "ip -6 route add %s via fd00:%d:2::5" % (i, env.sv.pid, i, SERVER_NET, i))
    env.sv.sh("ip link set w%d up && ip -6 addr add fd00:%d:2::5/64 dev w%d nodad && "
              "ip -6 route add default via fd00:%d:2::1 dev w%d table %d"
              % (i, i, i, i, i, 100 + i))


@scenario(gateways=2)
def uplink_recreate(env):
    """g0's uplink device is created anew. The daemon attaches the programs
    to the new device as soon as it appears and writes the alive element for
    its new index within a refresh; old and new flows work, and the flows
    from before are still in the map."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()
    env.check("replies pass (of 3)", replies(env, f), 3)
    g0.tick()
    recreate_wan(env, g0)
    f.reroute()
    env.wait_for("the daemon attached again at once (a link notification, not its tick)", 1,
                 lambda: g0.log().count("attached to uplink") >= 2, step=0.05)
    env.wait_for("the alive element for the new device within a refresh", 5, g0.alive, step=0.2)
    env.check("the old flow passes again without a new packet out (of 3)", replies(env, f), 3)
    n = env.flow("udp", fw=g0, rev=g1)
    n.send()
    synced(env, n)
    env.check("a new flow is learned and announced", replies(env, n), 3)


@scenario(gateways=2)
def dead_uplink(env):
    """No daemon, and the uplink device is created anew: no programs, the
    table in place. The new device's index is not in the alive set, so what
    leaves through it is tracked from the first packet and conntrack carries
    a new symmetric flow at once; the old index's element expires on its own.
    Nothing waits for a human. (Fails on the unconditional notrack table.)"""
    g0 = env.g[0]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()
    env.check("replies pass (of 3)", replies(env, f), 3)
    for g in env.g:
        g.stop()
    recreate_wan(env, g0)
    f.reroute()
    env.check("the table is still there, no programs on the new device",
              [own_table(g0), "3780" in g0.node.run("tc", "filter", "show", "dev", "wan0",
                                                     "egress", check=False)], [True, False])
    n = env.flow("udp", fw=g0, rev=g0)
    n.send()
    env.check("a new symmetric flow passes on conntrack at once (of 3)", replies(env, n), 3)
    env.check("conntrack accepted them", g0.fwc("est"), 3)
    env.wait_for("the old index's element expires", 12,
                 lambda: not re.search(r"timeout \d", g0.node.run("nft", "list", "set", "ip6",
                                                                "flowsync", "alive", check=False)),
                 step=0.5)
    env.start()
    env.wait_for("the daemon is back, and the element for the new device", 5, g0.alive, step=0.2)
    env.check("nothing rejected", rejects(env), 0)


def cpu_ticks(g):
    """user + system clock ticks the daemon has used"""
    pid = children(g.proc.pid)[0]         # ptyrun's child: the daemon
    with open("/proc/%d/stat" % pid) as st:
        f = st.read().rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])


def rule_present(g):
    return "flowsync" in g.node.run("nft", "list", "chain", "inet", "fw", "forward", check=False)


@scenario(gateways=2)
def rules(env):
    """What the datapath needs is put back when it disappears: the accept
    rule after a firewall reload without it, the notrack table with its alive
    element, and the
    programs on the uplink."""
    g0 = env.g[0]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()                # learned before the first reply, whatever the neighbours' timing
    env.sleep(0.2)
    lp = env.loop(100, 1, f.send)
    env.check("replies pass (of 3)", replies(env, f), 3)

    g0.node.sh("nft delete table inet fw")
    g0.node.nft(g0.ruleset(accept_rule=False))
    env.wait_for("the accept rule is back after a reload without it", 2,
                 lambda: rule_present(g0), step=0.1)
    env.check("replies pass again (of 3)", replies(env, f), 3)
    env.check("the daemon says so", g0.log(), "accept rule for mark 0x01000000 added")
    chain = g0.node.run("nft", "list", "chain", "inet", "fw", "forward")
    env.true("it sits behind the MSS clamp and before the established rule",
             chain.index("maxseg") < chain.index("flowsync") < chain.index("ct state"))

    g0.node.sh("nft delete table ip6 flowsync")
    env.wait_for("the notrack table is back", 2, lambda: own_table(g0), step=0.1)
    env.wait_for("with the alive element", 5, g0.alive, step=0.2)

    tcx(g0, "detach", "ingress", "fs_ingress")
    tcx(g0, "detach", "egress", "fs_egress")
    env.wait_for("the programs are back at the next tick (detaching sends no notification)",
                 env.I + 1, lambda: "the programs were gone" in g0.log(), step=0.05)
    env.check("replies pass again (of 3)", replies(env, f), 3)
    env.wait_st("status shows it", g0, "dp_attached", 2)
    env.check("status: attached and rules in place", [g0.st("attached"), g0.st("fw_ok")], [1, 1])
    lp.stop()


@scenario(gateways=2)
def rules_idle(env):
    """A gateway in order does not run nft: the rules are looked at when the
    ruleset changes, not every interval."""
    g0 = env.g[0]

    def looks():
        return g0.log().count("rules: looked")

    env.start()
    env.sleep(1.5)
    n0 = looks()
    env.check("the start looked at the rules", n0, lambda n: 1 <= n <= 2)
    env.sleep(2 * env.I + 1)
    env.check("no look in the two intervals since", looks() - n0, 0)
    g0.node.sh("nft add table inet other")
    env.wait_for("somebody changes the ruleset: a look", 2, lambda: looks() - n0 == 1, step=0.1)
    g0.node.sh("nft delete table inet other")
    env.sleep(2)
    env.check("and one for the next change, no more", looks() - n0, 2)
    env.check("status: rules in place", g0.st("fw_ok"), 1)


@scenario(gateways=2)
def bootstrap(env):
    """The first start on a gateway that tracked its flows with conntrack: the
    accept rule, the programs, the table and the alive element come at once.
    A flow that was running keeps passing: on conntrack until its next packet
    puts it in the map, on the mark from then on. Nothing is rejected, no
    interval passes."""
    g0 = env.g[0]
    g0.node.sh("nft delete table ip6 flowsync; nft delete table inet fw")
    g0.node.nft(g0.ruleset(accept_rule=False))
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()
    env.check("before: conntrack accepts the replies (of 3)", replies(env, f), 3)
    env.check("accepted as established", g0.fwc("est"), 3)
    d0 = f.delivered("rev")
    rl = env.loop(1000, 0.1, lambda: f.send("rev"))    # the client stays silent
    env.start()
    env.wait_for("the table and the alive element are there within a second", 2,
                 lambda: own_table(g0) and g0.alive(), step=0.1)
    env.sleep(2)
    env.check("the silent flow's replies kept arriving, on conntrack",
              f.delivered("rev") - d0, lambda n: n >= 15)
    f.send()                                            # learned, untracked from here
    env.wait_for("the flow is in the map", 1, lambda: g0.ft(f).local, step=0.05)
    env.sleep(0.5)
    rl.stop()
    env.sleep(0.3)
    env.check("nothing rejected over the change", g0.fwc("rej"), 0)
    e0 = g0.fwc("est")
    env.check("replies pass on the mark now (of 3)", replies(env, f), 3)
    env.check("conntrack no longer sees them", g0.fwc("est") - e0, 0)


@scenario(gateways=2)
def forged(env):
    """Records from an address that is not a peer are dropped, and a full
    remote map refuses new flows while it keeps refreshing the ones it has."""
    g0, g1 = env.g[:2]
    env.start("-C", 1024, debug=False)
    env.hub.sh("ip addr add 10.0.0.9/24 dev br0")
    env.probe(env.hub, "spoof", "::ffff:10.0.0.9", "::ffff:" + g1.addr, PORT, 50, 1)
    env.wait_st("g1 counted the stranger's datagrams", g1, "rx_bad_peer", 40)
    env.check("and holds nothing", g1.flows("remote"), [])

    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(100, 1, f.send)
    synced(env, f)
    b = env.bulk(1500, fw=g0, rev=g1)
    b.flood()
    env.wait_st("g1 refused flows beyond max_remote", g1, "rx_limited", 1)
    n = len(g1.flows("remote"))
    env.check("g1 holds max_remote at most", n, lambda n: 900 <= n <= 1024)
    env.hold("the flow it had is still refreshed", env.E + env.I, lambda: g1.ft(f).remote)
    env.check("replies pass (of 3)", replies(env, f), 3)
    env.ok("g1: %s" % re.sub(r"\s+", " ", g1.status_line("rx_limited")))
    lp.stop()


@scenario(gateways=2)
def unprivileged(env):
    """The daemon as an ordinary user with CAP_NET_ADMIN only (--user, as the
    init script starts it): it still syncs, puts a removed rule back (nft
    runs with the same capability) and attaches the programs again."""
    g0, g1 = env.g[:2]
    env.start("-u", "nobody")
    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(100, 1, f.send)
    synced(env, f)
    env.check("replies pass g1 (of 3)", replies(env, f), 3)
    pid = children(g1.proc.pid)[0]         # ptyrun's child: the daemon
    with open("/proc/%d/status" % pid) as st:
        uid = [ln.split()[1] for ln in st if ln.startswith("Uid:")][0]
    env.check("g1's daemon does not run as root (uid)", uid, lambda u: u != "0")
    g1.node.sh("nft delete table inet fw")
    g1.node.nft(g1.ruleset(accept_rule=False))
    env.wait_for("g1 put the accept rule back", 2, lambda: rule_present(g1), step=0.1)
    tcx(g1, "detach", "ingress", "fs_ingress")
    env.wait_for("and attached the programs again at its tick", env.I + 1,
                 lambda: "the programs were gone" in g1.log(), step=0.05)
    env.check("replies pass g1 again (of 3)", replies(env, f), 3)
    lp.stop()


def forward_chain(g):
    return g.node.run("nft", "list", "chain", "inet", "fw", "forward", check=False)


def own_table(g):
    return g.node.ok("nft", "list", "table", "ip6", "flowsync")


@scenario(gateways=2)
def mark_mismatch(env):
    """The daemon runs with another mark than the firewall's accept rule
    names (option mark changed, the package's include not). The daemon
    replaces the rule: what it marks is accepted, and the other mark no
    longer opens the firewall."""
    g0 = env.g[0]
    env.start("-m", "0x02000000")
    f = env.flow("udp", fw=g0, rev=g0)
    lp = env.loop(100, 1, f.send)
    env.wait_for("the accept rule names the daemon's mark", 2,
                 lambda: "0x02000000" in forward_chain(g0), step=0.1)
    env.check("the rule for the other mark is gone", "0x01000000" in forward_chain(g0), False)
    env.check("replies pass (of 3)", replies(env, f), 3)
    env.check("the daemon names the file to fix", g0.log(), "10-flowsync.nft")
    lp.stop()


@scenario(gateways=2)
def fail_open(env):
    """The programs cannot be attached (the uplink's egress hook is full of
    foreign programs). The daemon keeps the alive element out of the set, so
    what leaves is tracked and conntrack carries the flows on a symmetric
    path, as before flowsync; the stale element of the last run is taken away
    at the first look. Once the programs are back, the element follows within
    a refresh."""
    g0 = env.g[0]
    env.start()
    g0.stop()           # or it would have its program back before the hook is full
    tcx(g0, "detach", "egress", "fs_egress")
    tcx(g0, "fill", "egress")
    g0.start()
    env.wait_for("the daemon cannot attach", 3, lambda: "attach egress" in g0.log(), step=0.1)
    c0 = cpu_ticks(g0)
    env.wait_for("the table is there, the alive element is not", 3,
                 lambda: own_table(g0) and not g0.alive(), step=0.2)
    env.check("the daemon says so", g0.log(), "alive element removed")
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()
    env.check("a new symmetric flow passes (of 3)", replies(env, f), 3)
    env.check("conntrack accepted the replies", g0.fwc("est"), 3)
    g0.tick()
    env.check("status: not attached, rules in place, not alive",
              [g0.st("attached"), g0.st("fw_ok"), g0.st("alive")], [0, 1, 0])
    # a daemon that retries on every notification of its own half-done attach
    # burns a CPU (about 90 ticks a second); its log would not show it
    env.check("the daemon does not spin on its own failed attempts (CPU ticks while "
              "it could not attach)", cpu_ticks(g0) - c0, lambda n: n < 30)

    tcx(g0, "detach", "egress", "dptest_dummy")
    env.wait_for("the programs are back at the next tick", env.I + 1,
                 lambda: g0.log().count("attached to uplink") >= 2, step=0.05)
    env.wait_for("the alive element follows within a refresh", 5, g0.alive, step=0.2)
    lp = env.loop(100, 0.5, f.send)
    env.sleep(1)
    e0 = g0.fwc("est")
    env.check("replies pass, and not on conntrack any more (of 3)",
              [replies(env, f), g0.fwc("est") - e0], [3, 0])
    env.check("nothing was rejected", g0.fwc("rej"), 0)
    lp.stop()


@scenario(gateways=2)
def load_failure(env):
    """A daemon that cannot load its programs (here: no object file) leaves
    the programs of the last run on the uplink with their flow tables and the
    table; the alive element expires on its own, so what leaves is tracked.
    The next start that works finds every flow and refreshes the element."""
    g0, g1 = env.g[:2]
    env.start()
    fa = env.flow("udp", fw=g0, rev=g1)
    fs = env.flow("udp", fw=g1, rev=g1)
    fa.send()
    fs.send()
    synced(env, fa)
    g1.stop()
    g1.start("--bpf-object", "/nonexistent/flowsync.o")
    env.wait_for("the daemon says why it gives up", 3,
                 lambda: "/nonexistent/flowsync.o" in g1.log(), step=0.1)
    env.wait_for("and is gone", 3, lambda: not children(g1.proc.pid), step=0.1)
    env.check("the table stays", own_table(g1), True)
    env.check("the flow tables are still there: the peer's flow, its own",
              [g1.ft(fa).remote, g1.ft(fs).local], [True, True])
    env.check("replies pass on the last run's programs, both flows (of 6)",
              replies(env, fa) + replies(env, fs), 6)
    env.wait_for("the alive element expires: nobody refreshes it", 12, lambda: not g1.alive(),
                 step=0.5)
    env.check("nothing rejected", rejects(env), 0)
    g1.start()
    env.wait_for("a start that works", 5, g1.up)
    env.check("its own flow is still there", g1.ft(fs).local, True)
    env.wait_for("the peer's flow is back (resync)", 3, lambda: g1.ft(fa).remote, step=0.05)
    env.wait_for("the alive element is back", 5, g1.alive, step=0.2)


@scenario(gateways=2)
def resize(env):
    """max_remote changes: the remote table cannot be reused and is made
    anew (the peers fill it again on request), the local one is kept."""
    g0, g1 = env.g[:2]
    env.start()
    fa = env.flow("udp", fw=g0, rev=g1)
    fs = env.flow("udp", fw=g1, rev=g1)
    fa.send()
    fs.send()
    synced(env, fa)
    g1.restart("-C", 4096)
    env.wait_for("g1 is up again", 5, g1.up)
    env.check("g1 says which table it replaced", g1.log(), "fs_remote was pinned with another size")
    env.check("its own flow is still there", g1.ft(fs).local, True)
    env.wait_for("the peer's flow is back (resync)", 3, lambda: g1.ft(fa).remote, step=0.05)
    env.check("replies pass, both flows (of 6)", replies(env, fa) + replies(env, fs), 6)


@scenario(gateways=2)
def ingress_qdisc(env):
    """SQM on the uplink: an ingress qdisc with a redirect to an IFB, a
    shaper as the root qdisc. The programs sit on the device's tcx hooks,
    not on a qdisc, so both are there at the same time, whichever came
    first; the ingress qdisc can be removed and re-added under them, and the
    daemon never has to attach again."""
    g0 = env.g[0]
    sqm = ("ip link add ifb0 type ifb && ip link set ifb0 up && "
           "tc qdisc add dev ifb0 root fq_codel && "
           "tc qdisc replace dev wan0 root fq_codel && "
           "tc qdisc add dev wan0 handle ffff: ingress && "
           "tc filter add dev wan0 parent ffff: protocol all matchall "
           "action mirred egress redirect dev ifb0")
    g0.node.sh(sqm)
    env.start()
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()
    env.sleep(0.2)
    env.check("replies pass with SQM in place first (of 3)", replies(env, f), 3)
    env.check("both programs are on the hooks, SQM's qdiscs stay",
              [hook_names(g0, "ingress"), hook_names(g0, "egress"),
               "ingress" in g0.node.run("tc", "qdisc", "show", "dev", "wan0")],
              [["fs_ingress"], ["fs_egress"], True])
    env.wait_for("the alive element", 5, g0.alive, step=0.2)
    env.check("the redirect to the IFB sees the replies (SQM shapes what is not bypassed)",
              g0.node.run("tc", "-s", "qdisc", "show", "dev", "ifb0"),
              lambda s: int(re.search(r"Sent \d+ bytes (\d+) pkt", s).group(1)) >= 3
              if not env.bypass else True)

    g0.node.sh("tc qdisc del dev wan0 ingress")
    env.check("without the ingress qdisc the programs are still there",
              hook_names(g0, "ingress"), ["fs_ingress"])
    env.check("replies pass (of 3)", replies(env, f), 3)
    g0.node.sh("tc qdisc add dev wan0 handle ffff: ingress && tc filter add dev wan0 parent "
               "ffff: protocol all matchall action mirred egress redirect dev ifb0")
    env.check("and with it back (of 3)", replies(env, f), 3)
    g0.tick()
    env.check("status: attached throughout, never attached again",
              [g0.st("attached"), g0.log().count("attached to uplink")], [1, 1])
    env.check("nothing rejected", rejects(env), 0)

    # the other order: the daemon first, SQM on a running datapath
    g1 = env.g[1]
    g1.node.sh(sqm)
    f1 = env.flow("udp", fw=g1, rev=g1)
    f1.send()
    env.sleep(0.2)
    env.check("replies pass with SQM added under the programs (of 3)", replies(env, f1), 3)
    env.check("the programs stayed", hook_names(g1, "ingress") + hook_names(g1, "egress"),
              ["fs_ingress", "fs_egress"])
