"""The gateways' stateful firewall on conntrack, shared by the implementations
that keep conntrack as the state (none, flowsync_conntrack, conntrackd).

The forward chain as fw4 renders it on the gateways: policy reject, established
accept, the mesh may go anywhere; per policy the rate-limited stateless accept
for TCP segments with ACK or RST (bbb-configs) and flow offloading.
The conntrack timeouts are production's, divided by the lab's scale (timers.py).
"""
import sys

FLUSH = ("import socket, struct; s = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 12); "
         "s.send(struct.pack('=IHHIIBBH', 20, 0x0102, 5, 1, 0, 10, 0, 0)); s.recv(4096)")


# the stateless accept of bbb-configs, with a count per flow of what it let through
BYPASSED_SET = "\tset bypassed {\n\t\ttype ipv6_addr . ipv6_addr; flags dynamic; counter; size 65536;\n\t}"
BYPASS = ("\t\tmeta nfproto ipv6 tcp flags & ack == ack limit rate 5000/second burst 2500 packets "
          "update @bypassed { ip6 saddr . ip6 daddr } counter name fwd_ack accept\n"
          "\t\tmeta nfproto ipv6 tcp flags & rst == rst limit rate 1000/second burst 500 packets "
          "update @bypassed { ip6 saddr . ip6 daddr } accept")


def install(gw, extra_tables=""):
    gw.node.sysctl(**{"net.netfilter.nf_conntrack_" + k: v for k, v in gw.timers.conntrack().items()},
                   **{"net.netfilter.nf_conntrack_checksum": 0})
    p = gw.policy
    gw.node.nft("""
table inet fw {
	counter fwd_ack {}
	counter fwd_rej {}
%(set)s
%(ft)s
	chain forward {
		type filter hook forward priority 0; policy drop;
%(offload)s
		ct state established,related accept
		iifname "mesh0" accept
%(bypass)s
		counter name fwd_rej
		meta l4proto tcp reject with tcp reset
		reject
	}
}
%(extra)s
""" % dict(
        ft="\tflowtable ft {\n\t\thook ingress priority 0; devices = { mesh0, wan0 };\n\t}"
           if p["offload"] else "",
        offload="\t\tmeta l4proto { tcp, udp } flow offload @ft" if p["offload"] else "",
        set=BYPASSED_SET, bypass=BYPASS if p["bypass"] else "",
        extra=extra_tables))


def flush(gw):
    """conntrack -F for IPv6"""
    gw.node.run(sys.executable, "-c", FLUSH, check=False)
