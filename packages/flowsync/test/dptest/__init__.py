"""dptest: what is specific to flowsync's datapath, tested from inside a gateway.

gwlab (../gwlab) judges whether a fleet delivers, from the endpoints, for any
implementation. These scenarios look at what it cannot see from there: the
flow tables, the tc programs and the nftables rules, fragments and extension
headers, the bypass, protocols without ports, what the daemon puts back when
it disappears. Two or three gateways between a client and a server namespace,
no link latency, as root (BPF programs cannot be loaded from a user
namespace).

  python3 -m dptest --flowsync PATH --bpf-object PATH [--scenarios ...]
"""
