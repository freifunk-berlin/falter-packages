"""The status quo: conntrackd + samplicator as bbb-configs deploys them.
conntrackd (NOTRACK, both caches off, StartupResync) sends every conntrack
event to 127.0.0.1:3780, an nft output NAT rule redirects that to samplicator
on port 2000, which copies each datagram to the peers' conntrackd.

Options: conntrackd=<binary>, samplicate=<binary> (default: from PATH).
In an unprivileged user namespace conntrackd 1.4.8 needs
../conntrackd-userns-sockbuf.patch (flowsync-statusquo branch) to start."""
import os
import shutil
import tempfile

from . import Impl, ctfw
from ..ns import kill

PORT, SAMPLICATOR_PORT = 3780, 2000

CONF = """Sync {
    Mode NOTRACK {
        DisableExternalCache On
        DisableInternalCache On
        StartupResync on
    }
    UDP {
        IPv4_address %(addr)s
        IPv4_Destination_Address 127.0.0.1
        Port %(port)d
        Interface %(dev)s
        SndSocketBuffer 10485760
        RcvSocketBuffer 41943040
        Checksum on
    }
}
General {
    HashSize 65536
    HashLimit 1048576
    LogFile %(log)s
    Syslog off
    LockFile %(run)s/lock
    NetlinkBufferSize 20971520
    NetlinkBufferSizeMaxGrowth 83886080
    UNIX {
        Path %(run)s/ctl
    }
    Filter From Userspace {
        Protocol Accept {
            tcp
            udp
            ipv6-icmp
        }
        Address Ignore {
            IPv4_address 0.0.0.0/0
            IPv6_address ::1
        }
    }
}
"""

REDIRECT = """
table inet samplicator {
	chain to_samplicator {
		type nat hook output priority -100; policy accept;
		oif lo udp dport %d counter redirect to :%d
	}
}
""" % (PORT, SAMPLICATOR_PORT)


class IMPL(Impl):
    def __init__(self, opts):
        super().__init__(opts)
        self.bin = {k: os.path.abspath(opts[k]) if k in opts else shutil.which(k)
                    for k in ("conntrackd", "samplicate")}
        for k, v in self.bin.items():
            if not v:
                raise SystemExit("conntrackd: no %s binary (--set %s=PATH)" % (k, k))
        self.procs, self.run = {}, {}

    def install(self, gw):
        ctfw.install(gw, REDIRECT)
        # the UNIX socket path is limited to 107 bytes: a short directory
        self.run[gw.name] = tempfile.mkdtemp(prefix="gwlab-")
        with open(os.path.join(gw.dir, "conntrackd.conf"), "w") as f:
            f.write(CONF % dict(addr=gw.addr4, port=PORT, dev=gw.uplink_if, log=gw.log,
                                run=self.run[gw.name]))
        with open(os.path.join(gw.dir, "samplicator.conf"), "w") as f:
            f.write("127.0.0.1:%s\n" % "".join(" %s/%d" % (p, PORT) for p in sorted(gw.peers4)))

    def start(self, gw):
        self.procs[gw.name] = [
            gw.spawn(self.bin["samplicate"], "-c", os.path.join(gw.dir, "samplicator.conf"),
                     "-p", SAMPLICATOR_PORT),
            gw.spawn(self.bin["conntrackd"], "-C", os.path.join(gw.dir, "conntrackd.conf")),
        ]

    def stop(self, gw):
        for p in self.procs.pop(gw.name, []):
            kill(p)
        shutil.rmtree(self.run.get(gw.name, ""), ignore_errors=True)

    def lose_state(self, gw):
        ctfw.flush(gw)
