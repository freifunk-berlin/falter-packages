"""What a flow does. The kinds are implemented in agent.py.

The first group is written in production time and runs faster by the lab's
scale (seconds and intervals divided, packet rates multiplied): at the default
scale of 10 a flow that talks every 10 s for 10 minutes talks every second
for one. The rest is real time."""
TRAFFIC = dict(
    # five separate connections, a request and a response each (web)
    tcp_short=dict(kind="tcp_short", connections=5, every=20, request=200, response=2000),
    # one connection talking every 10 s for ten minutes (ssh, a tunnel)
    tcp_talk=dict(kind="tcp_talk", seconds=600, every=10),
    # one connection talking for 2.2 established timeouts (scenario longlived)
    tcp_long=dict(kind="tcp_talk", seconds=2.2 * 7440, every=10),
    # a connection that falls silent for ten minutes, twice: first the server
    # speaks again (a push, IMAP IDLE), then the client (an ssh session left open)
    tcp_idle=dict(kind="tcp_idle", idle=600),
    # ask, get an answer, ten times (an API over UDP)
    udp_rr=dict(kind="udp_rr", requests=10, every=10),
    # both ends keep sending for 2.8 UDP stream timeouts (a call, a QUIC download)
    udp_stream=dict(kind="udp_stream", up_pps=1, down_pps=2, seconds=500),
    # how late may the first answer be for the return gateway to know the flow?
    ladder=dict(kind="udp_ladder", delays_ms=[0, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000]),

    # measurements, for scenarios that run alone
    # small packets as fast as one core pushes them through the path
    flood_up=dict(kind="udp_flood", dir="up", seconds=5, size=64),
    flood_down=dict(kind="udp_flood", dir="down", seconds=5, size=64),
    # one TCP download at full speed
    bulk=dict(kind="tcp_bulk", seconds=8),
    # new flows at a rising rate; the server answers each 50 ms later
    newflows=dict(kind="udp_newflows", rates=[500, 1000, 2000, 4000, 8000, 12000],
                  step_seconds=2, reply_delay=0.05),
)
