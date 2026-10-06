"""What a flow does. The kinds are implemented in agent.py."""
TRAFFIC = dict(
    # five separate connections, a request and a response each (web)
    tcp_short=dict(kind="tcp_short", connections=5, every=2, request=200, response=2000),
    # one connection talking every second for a minute (ssh, a tunnel)
    tcp_talk=dict(kind="tcp_talk", seconds=60, every=1),
    # ask, get an answer, ten times (an API over UDP)
    udp_rr=dict(kind="udp_rr", requests=10, every=1),
    # a call or a QUIC download: both ends send for 50 s
    udp_stream=dict(kind="udp_stream", up_pps=10, down_pps=20, seconds=50),
)
