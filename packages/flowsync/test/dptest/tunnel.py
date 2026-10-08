"""Tunnel devices on the mesh side of a gateway, as the gateways have them:
an IPv4 GRE tunnel to the other gateways (gre4-*, MTU 1476) and WireGuard
from the corerouters (wg_*, MTU 1280). Both ride the mesh veth as IPv4 and
carry the client's IPv6.

As a script (run inside a namespace by mesh_tunnel):
  tunnel.py wgconf <dev> <private key> <listen port> <peer public key> <endpoint addr> <endpoint port>
"""
import base64
import os
import sys

GRE_MTU = 1476          # the gateways' gre4-* devices
WG_MTU = 1280           # what the corerouters set on their wg tunnels
WG_PORT = 51820         # + the gateway's index: CL ends two tunnels on one address


def _keypair():
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import x25519

    priv = x25519.X25519PrivateKey.generate()
    raw = serialization.Encoding.Raw, serialization.PublicFormat.Raw
    pub = priv.public_key().public_bytes(*raw)
    sec = priv.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                             serialization.NoEncryption())
    return base64.b64encode(sec).decode(), base64.b64encode(pub).decode()


def wgconf(dev, private_key, listen_port, peer_public_key, endpoint_addr, endpoint_port):
    from pyroute2 import WireGuard

    with WireGuard() as wg:
        wg.set(dev, private_key=private_key, listen_port=int(listen_port),
               peer={"public_key": peer_public_key, "endpoint_addr": endpoint_addr,
                     "endpoint_port": int(endpoint_port), "allowed_ips": ["::/0"]})


def mesh_tunnel(env, g, kind):
    """a gre or wg tunnel between gateway g (tun<i>) and CL (tun<i>c) over
    the mesh veth, IPv4 outer 10.1.<i>.0/30; returns the undo. Skips the
    scenario when the kernel has no such device type."""
    from .check import Skip

    i = g.i
    gn, cl = g.node, env.cl
    gdev, cdev = "tun%d" % i, "tun%dc" % i
    ga, ca = "10.1.%d.1" % i, "10.1.%d.2" % i
    gn.sh("ip addr replace %s/30 dev mesh0" % ga)
    cl.sh("ip addr replace %s/30 dev m%d" % (ca, i))
    if kind == "gre":
        mk = "ip link add %s type gre local %s remote %s ttl 64"
        if not gn.ok("sh", "-c", mk % (gdev, ga, ca)):
            raise Skip("no gre devices (modprobe ip_gre)")
        cl.sh(mk % (cdev, ca, ga))
        mtu = GRE_MTU
    elif kind == "wg":
        if not gn.ok("ip", "link", "add", gdev, "type", "wireguard"):
            raise Skip("no wireguard devices (modprobe wireguard)")
        cl.run("ip", "link", "add", cdev, "type", "wireguard")
        gsec, gpub = _keypair()
        csec, cpub = _keypair()
        here = os.path.abspath(__file__)
        port = WG_PORT + i
        gn.run(env.python, here, "wgconf", gdev, gsec, port, cpub, ca, port)
        cl.run(env.python, here, "wgconf", cdev, csec, port, gpub, ga, port)
        mtu = WG_MTU
    else:
        raise ValueError(kind)
    gn.sh("ip link set %s mtu %d up" % (gdev, mtu))
    cl.sh("ip link set %s mtu %d up" % (cdev, mtu))

    def undo():
        gn.sh("ip link del %s; ip addr del %s/30 dev mesh0" % (gdev, ga), check=False)
        cl.sh("ip link del %s; ip addr del %s/30 dev m%d" % (cdev, ca, i), check=False)
    return undo


def route_via(env, f):
    """the flow takes the tunnels: out of CL through the forward gateway's,
    back to the client through the reverse gateway's (Flow.close() removes
    both routes)"""
    env.cl.run("ip", "-6", "route", "replace", f.s + "/128", "dev", "tun%dc" % f.fw.i)
    f.rev.node.run("ip", "-6", "route", "replace", f.c + "/128", "dev", "tun%d" % f.rev.i)


if __name__ == "__main__":
    if sys.argv[1:2] == ["wgconf"]:
        wgconf(*sys.argv[2:8])
    else:
        raise SystemExit(__doc__)
