# gwlab: data path tests for a gateway fleet

Does the network deliver when a flow leaves through one gateway and comes
back through another? gwlab builds a lab of clients, gateways and servers
with per-link latency, runs real traffic over every gateway pair at once and
judges each flow by what its endpoints saw. It does not know how the gateways
share their state: that is an implementation (`impl/`), and the same
scenario runs against any of them.

    cd packages/flowsync/test
    python3 -m gwlab steady --impl flowsync                       # ../src/flowsync
    python3 -m gwlab steady --impl none --fleet strict            # no sync: asymmetric flows must fail
    python3 -m gwlab steady --impl conntrackd --set conntrackd=PATH --set samplicate=PATH
    python3 -m gwlab steady --impl flowsync --fleet strict --flow 'tcp_talk:A>gw1>B>gw2'   # one flow
    VNG=/path/to/vng gwlab/vm.sh OUT steady --impl flowsync_bpf --set bin=PATH --set bpf_object=PATH

Python 3 stdlib, iproute2, tc (netem), nft, util-linux. No root: every lab
runs under `unshare -Urn`, in a systemd scope so that CPU is counted per
gateway. Implementations that need real root (BPF) run through `vm.sh`
(virtme-ng: a throwaway VM on the host's kernel); so can all the others.

## What you read and write

| File | What it is |
|---|---|
| `scenarios/gw5.py` | the topology (five gateways, ms per link, two clients, two servers) and the fleets: which gateway has the stateless ACK/RST bypass, which offloads |
| `scenarios/traffic.py` | what a flow does: `tcp_short`, `tcp_talk`, `udp_rr`, `udp_stream` |
| `scenarios/steady.py` | a scenario: the flows as a grid of traffic x client x server x forward gateway x return gateway |
| `expect.py` | what "works" means, for any implementation |
| `impl/*.py` | one file per implementation: `none`, `flowsync`, `conntrackd`, `flowsync_bpf` |

A flow is named `traffic:client>forward gateway>server>return gateway`.

**The expectation** (`expect.py`): a flow through one gateway is clean from
its first packet. A flow through two gateways may lose its first attempt (the
server's answer can be faster than the gateways' sync); the endpoint's own
retry has to repair that (the next UDP request, a TCP SYN retransmission), and
after 1.5 s the flow has to be clean. Nothing may reset a connection. Results
are pass or FAIL; how many flows needed the retry is a column.

**An implementation** gets a gateway (a namespace with `mesh0`, `wan0`, the
peers' sync addresses on the uplink, a policy, a directory) and provides
`install`, `start`, `stop`, `lose_state`. It installs its own firewall for
the policy; the lab never looks inside a gateway.

## The lab

```
   client A      client B
        \          /
     [ mesh bridge ]              fd00:1::/64
     |3   |8   |10  |3   |1       ms, mesh link
    gw1  gw2  gw3  gw4  gw5
     |6   |6   |3   |2   |5       ms, uplink: user traffic and sync (10.0.0.0/24)
     [ internet bridge ]          fd00:2::/64
        /          \
   server A (3)   server B (5)
```

Every link is a veth with netem delay on both ends; the bridges add nothing,
so a path's delay is the sum of its links (a connect over A>gw1>B>gw2 takes
the 33 ms the numbers add up to). Every flow has a client and a server
address of its own; the client routes the server's via the forward gateway,
the server the client's via the return gateway.

The sync travels over the uplink like the traffic, so the server's answer
reaches the return gateway 2 x server distance after the sync could: 6 and
10 ms here. An implementation slower than that loses the race and the flow
needs its retry.

## The report

Per fleet: one row per traffic, path (sym/asym) and the return gateway's
rules, with flows, pass, FAIL, needed retry, packets lost per direction, the
longest gap in a UDP stream, connect time. Then one row per gateway: CPU time
of the implementation's processes (user / system, from a cgroup) and what it
sent on the sync path. `results.json` per fleet has every flow's raw result;
each gateway's log is next to it.

Reading the numbers: CPU is relative (this host, not a router), timers are
scaled about ten times shorter than production's, and the conntrack hash
table is shared by all namespaces of the host, which makes every table dump
cost a walk of the host's buckets.

## Not there yet

- Scenarios with events: reroute, state loss (`lose_state` exists), a sync
  blackout, the uplink re-created.
- Measurement scenarios, run alone: per-packet cost, bulk goodput, new flows
  per second.
- Kernel packet-path CPU per gateway (the cgroup only sees processes, and a
  BPF data path has its cost in the kernel).
- Sync latency measured black-box (a server that delays its first answer).
