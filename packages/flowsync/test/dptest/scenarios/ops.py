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
