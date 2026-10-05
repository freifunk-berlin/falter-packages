"""The sync device going away and coming back under the daemon."""
import re

from ..gateway import children
from ..scenario import scenario


def ifindex(g, dev="eth0"):
    out = g.node.run("ip", "-o", "link", "show", "dev", dev, check=False)
    m = re.match(r"(\d+):", out)
    return int(m.group(1)) if m else None


def del_uplink(env, g):
    """g's sync device goes away (its hub end with it)"""
    env.hub.sh("ip link del s%d" % g.i)


def add_uplink(env, g):
    """g's sync device comes (back) under the same name and address, with a new
    ifindex: what netifd does to a VLAN/bridge/bond uplink on ifdown/ifup or a
    network restart"""
    env.hub.sh("ip link add s%d type veth peer name eth0 netns %d && ip link set s%d master br0 up"
               % (g.i, g.node.pid, g.i))
    g.node.sh("ip link set eth0 up && ip addr add %s/24 dev eth0" % g.addr)


def running(g):
    return bool(g.proc and children(g.proc.pid))


def syncs(env, g0, g1):
    """both directions work between g0 and g1: each gets a copy of the other's
    new flow, and the reply passes on it"""
    f = env.flow("udp", fw=g0, rev=g1)
    lp = env.loop(4 * env.I, 1, f.send)
    env.wait_for("%s gets the copy of %s's new flow" % (g1, g0), 4 * env.I,
                 lambda: g1.ct(f).is_copy)
    f.send("rev")
    env.wait_for("the reply passes %s on the copy" % g1, 3, lambda: f.delivered("rev") >= 1)
    r = env.flow("udp", fw=g1, rev=g0)
    lp2 = env.loop(4 * env.I, 1, r.send)
    env.wait_for("%s gets the copy of %s's new flow" % (g0, g1), 4 * env.I,
                 lambda: g0.ct(r).is_copy)
    lp.stop()
    lp2.stop()


@scenario(gateways=2, once=True, tags={"netdev", "restart"})
def uplink_recreate(env):
    """g1's sync device (-I eth0) is deleted and created again with the same
    name and address. The daemon must go on syncing in both directions,
    without a restart, and its sends must stop failing."""
    g0, g1 = env.g[:2]
    env.start()
    f = env.flow("udp", fw=g0, rev=g1)
    f.send()
    env.wait_for("before: g1 has the copy of g0's flow", 3, lambda: g1.ct(f).is_copy)

    i0 = ifindex(g1)
    del_uplink(env, g1)
    add_uplink(env, g1)
    i1 = ifindex(g1)
    env.true("g1's eth0 came back with a new ifindex", i0 and i1 and i0 != i1,
             "%s -> %s" % (i0, i1))
    syncs(env, g0, g1)
    env.true("g1's daemon still runs", running(g1))
    g1.tick()
    e0 = g1.st("tx_errors") or 0
    g1.tick()
    g1.tick()
    env.check("g1's sends no longer fail", (g1.st("tx_errors") or 0) - e0, 0)


@scenario(gateways=2, once=True, tags={"netdev", "restart"})
def uplink_late(env):
    """g1's daemon starts while its sync device (-I eth0, with the bind
    address) does not exist yet, as at boot before netifd has set up a VLAN
    uplink. It must keep running and start syncing once the device is there."""
    g0, g1 = env.g[:2]
    del_uplink(env, g1)
    env.start(wait=False)
    env.wait_for("g0 up", 8, g0.up)
    env.sleep(2)
    env.true("g1's daemon runs without its device", running(g1))
    add_uplink(env, g1)
    env.wait_for("g1 up", 2 * env.I + 2, g1.up)
    syncs(env, g0, g1)
