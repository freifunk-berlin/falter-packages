"""Time in the lab: production's timers, divided by one scale.

Everything that is a timer is written in its production value: the gateways'
conntrack settings here, an implementation's own timers in its adapter, a
scenario's durations in its traffic. One factor (--scale, default 10) divides
them all, so a run takes minutes and what expires before what stays as on a
gateway.

What does not scale: link delays, the sync itself and the endpoints' TCP
retransmission timers are real time. And a timer cannot go below a second
(the kernel's conntrack timeouts and the daemons' options are whole seconds):
a value that would is set to 1 s and listed in the report.
"""

# OpenWrt on the gateways: /etc/sysctl.d/11-nf-conntrack.conf, kernel defaults for the rest
CONNTRACK = dict(
    udp_timeout=60, udp_timeout_stream=180,
    tcp_timeout_syn_sent=120, tcp_timeout_syn_recv=60,
    tcp_timeout_established=7440, tcp_timeout_unacknowledged=300,
    tcp_timeout_fin_wait=120, tcp_timeout_close_wait=60, tcp_timeout_last_ack=30,
    tcp_timeout_time_wait=120, tcp_timeout_close=10,
)


SYN_RETRY = 1       # seconds, real time: when a client sends its SYN again


class Timers:
    def __init__(self, scale):
        self.scale = scale
        self.clamped = {}           # name -> production value of what was set to 1 s
        self.rounded = {}           # name -> production value of what did not divide evenly

    def s(self, name, production):
        """a production timer (seconds) in lab time: whole seconds, at least 1"""
        v = production / self.scale
        if v < 1:
            self.clamped[name] = production
            return 1
        if v != round(v):
            self.rounded[name] = production
        return round(v)

    def broken(self):
        """why a run at this scale would not test what production does: the
        relations between timers that the scenarios rely on, each of which a
        too large scale destroys"""
        c, out = self.conntrack(), []
        if c["tcp_timeout_syn_sent"] <= SYN_RETRY:
            out.append("a half-open TCP connection (120 s) is forgotten before the client's first "
                       "SYN retry (1 s, real time): connecting over two gateways cannot be judged")
        if c["udp_timeout"] >= c["udp_timeout_stream"]:
            out.append("a UDP flow without an answer (60 s) lives as long as one with (180 s)")
        if c["tcp_timeout_unacknowledged"] >= c["tcp_timeout_established"]:
            out.append("TCP unacknowledged (300 s) is no shorter than established (7440 s)")
        return out

    def conntrack(self):
        return {k: self.s(k, v) for k, v in CONNTRACK.items()}

    def span(self, production):
        """a duration of a scenario (seconds, may be a fraction) in lab time"""
        return production / self.scale
