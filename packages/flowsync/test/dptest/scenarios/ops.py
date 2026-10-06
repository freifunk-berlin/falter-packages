"""Operations as seen inside a gateway: the datapath without its daemon, a
reboot, the uplink device created anew, rules and programs that disappear,
the first start, limits, privileges. What a fleet's flows make of such events
end to end is gwlab's."""
import re

from ..gateway import PORT, SERVER_NET, children
from ..scenario import scenario
from .common import rejects, replies, synced


@scenario(gateways=2)
def daemon_down(env):
    """All daemons stop. New flows on a symmetric path still work (the
    programs stay on the uplink), an asymmetric flow lives until its copy
    expires. After the restart the first round brings it back at once."""
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
    lp = env.loop(100, 1, fa.send)
    env.sleep(env.E + 2)
    env.check("after an element_timeout without announcements: rejected (of 2)",
              replies(env, fa, 2), 0)
    env.start()
    env.wait_for("restarted: g1 holds the flow again", 2, lambda: g1.ft(fa).remote, step=0.05)
    env.check("replies pass g1 again (of 3)", replies(env, fa), 3)
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
    to the new device as soon as it appears; old and new flows work, and the
    flows from before are still in the map."""
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
    env.check("the old flow passes again without a new packet out (of 3)", replies(env, f), 3)
    n = env.flow("udp", fw=g0, rev=g1)
    n.send()
    synced(env, n)
    env.check("a new flow is learned and announced", replies(env, n), 3)


def rule_present(g):
    return "flowsync" in g.node.run("nft", "list", "chain", "inet", "fw", "forward", check=False)


@scenario(gateways=2)
def rules(env):
    """What the datapath needs is put back when it disappears: the accept
    rule after a firewall reload without it, the notrack table, and the
    programs on the uplink."""
    g0 = env.g[0]
    env.start()
    f = env.flow("udp", fw=g0, rev=g0)
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

    g0.node.sh("nft delete table inet flowsync")
    env.wait_for("the notrack table is back", 2,
                 lambda: g0.node.ok("nft", "list", "table", "inet", "flowsync"), step=0.1)

    g0.node.sh("tc filter del dev wan0 ingress; tc filter del dev wan0 egress")
    env.wait_for("the programs are back at once", 1,
                 lambda: "the filters were gone" in g0.log(), step=0.05)
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
    accept rule and the programs come at once, the notrack table one interval
    later, when the running flows have sent a packet and are in the map. A
    busy flow sees no rejected packet over the change."""
    g0 = env.g[0]
    g0.node.sh("nft delete table inet flowsync; nft delete table inet fw")
    g0.node.nft(g0.ruleset(accept_rule=False))
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()
    env.check("before: conntrack accepts the replies (of 3)", replies(env, f), 3)
    env.check("accepted as established", g0.fwc("est"), 3)
    d0 = f.delivered("rev")
    lp = env.loop(100, 0.5, f.send)
    rl = env.loop(1000, 0.1, lambda: f.send("rev"))
    env.start()
    env.sleep(1)
    env.check("no notrack table in the first interval",
              g0.node.ok("nft", "list", "table", "inet", "flowsync"), False)
    env.wait_for("the notrack table is there after one interval", env.I + 3,
                 lambda: g0.node.ok("nft", "list", "table", "inet", "flowsync"), step=0.2)
    env.sleep(2)
    rl.stop()
    lp.stop()
    env.sleep(0.3)
    env.check("nothing rejected over the change", g0.fwc("rej"), 0)
    env.check("replies kept arriving", f.delivered("rev") - d0, lambda n: n >= 30)
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
    g1.node.sh("tc filter del dev wan0 ingress")
    env.wait_for("and attached the programs again", 1,
                 lambda: "the filters were gone" in g1.log(), step=0.05)
    env.check("replies pass g1 again (of 3)", replies(env, f), 3)
    lp.stop()


def forward_chain(g):
    return g.node.run("nft", "list", "chain", "inet", "fw", "forward", check=False)


def own_table(g):
    return g.node.ok("nft", "list", "table", "inet", "flowsync")


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
    """The programs cannot be attached (a foreign filter sits where the
    egress program belongs): nothing would learn new flows, and with forwarded
    IPv6 untracked every reply would be rejected. After one interval the
    daemon takes its notrack table away and conntrack carries the flows on a
    symmetric path, as before flowsync. Once the programs are back the table
    returns, again one interval later."""
    g0 = env.g[0]
    env.start()
    g0.stop()           # or it would have its program back before the other filter is in
    g0.node.sh("tc filter del dev wan0 egress; tc filter add dev wan0 egress prio 3780 "
               "protocol all matchall action ok")
    g0.start()
    env.wait_for("the daemon cannot attach", 3, lambda: "attach egress" in g0.log(), step=0.1)
    env.wait_for("the notrack table is gone after an interval", env.I + 3,
                 lambda: not own_table(g0), step=0.2)
    env.check("the daemon says so", g0.log(), "falling back to conntrack")
    f = env.flow("udp", fw=g0, rev=g0)
    f.send()
    env.check("a new symmetric flow passes (of 3)", replies(env, f), 3)
    env.check("conntrack accepted the replies", g0.fwc("est"), 3)
    g0.tick()
    env.check("status: not attached, rules not in place", [g0.st("attached"), g0.st("fw_ok")],
              [0, 0])
    env.check("the daemon does not spin on its own failed attempts (log lines)",
              g0.log().count("attach egress"), lambda n: n <= 3)

    g0.node.sh("tc filter del dev wan0 egress prio 3780")
    env.wait_for("the programs are back within moments, not at the next tick", 2.5,
                 lambda: g0.log().count("attached to uplink") >= 2, step=0.05)
    env.check("no notrack table yet: the flows must be learned first", own_table(g0), False)
    lp = env.loop(100, 0.5, f.send)
    env.wait_for("the notrack table is back after an interval", env.I + 3, lambda: own_table(g0),
                 step=0.2)
    e0 = g0.fwc("est")
    env.check("replies pass, and not on conntrack any more (of 3)",
              [replies(env, f), g0.fwc("est") - e0], [3, 0])
    env.check("nothing was rejected", g0.fwc("rej"), 0)
    lp.stop()


@scenario(gateways=2)
def load_failure(env):
    """A daemon that cannot load its programs (here: no object file) leaves
    the programs of the last run on the uplink with their flow tables, and
    hands forwarded IPv6 back to conntrack. The next start that works finds
    every flow."""
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
    env.check("it took the notrack table with it: nobody looks after the programs now",
              own_table(g1), False)
    env.check("the flow tables are still there: the peer's flow, its own",
              [g1.ft(fa).remote, g1.ft(fs).local], [True, True])
    env.check("replies pass, both flows (of 6)", replies(env, fa) + replies(env, fs), 6)
    g1.start()
    env.wait_for("a start that works", 5, g1.up)
    env.check("found both flows", [g1.ft(fa).remote, g1.ft(fs).local], [True, True])
    env.wait_for("the notrack table is back after an interval", env.I + 3, lambda: own_table(g1),
                 step=0.2)
    env.check("nothing rejected", rejects(env), 0)


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
