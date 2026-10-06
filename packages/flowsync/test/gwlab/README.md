# gwlab: data path tests for a gateway fleet

Does the network deliver when a flow leaves through one gateway and comes
back through another? gwlab builds a lab of clients, gateways and servers
with per-link latency, runs real traffic over every gateway pair at once and
judges each flow by what its endpoints saw. It does not know how the gateways
share their state: that is an implementation (`impl/`), and the same
scenario runs against any of them.

    cd packages/flowsync/test
    VNG=/path/to/vng gwlab/vm.sh OUT steady --impl flowsync       # ../src/flowsync and flowsync.o, in a VM
    sudo python3 -m gwlab steady --impl flowsync                  # the same as root on this host
    python3 -m gwlab steady --impl none --fleet strict            # no sync: asymmetric flows must fail
    python3 -m gwlab steady --impl conntrackd --set conntrackd=PATH --set samplicate=PATH
    python3 -m gwlab steady --impl flowsync_conntrack --set bin=PATH   # flowsync as it was on conntrack
    sudo python3 -m gwlab steady --impl flowsync --fleet strict --flow 'tcp_talk:A>gw1>B>gw2'   # one flow

Python 3 stdlib, iproute2, tc (netem), nft, util-linux. Without root every
lab runs under `unshare -Urn`, in a systemd scope so that CPU is counted per
gateway. flowsync itself needs real root (BPF programs cannot be loaded from
a user namespace): as root, or through `vm.sh` (virtme-ng: a throwaway VM on
the host's kernel), which works for all the others too.

## What you read and write

| File | What it is |
|---|---|
| `scenarios/gw5.py` | the topology (five gateways, ms per link, two clients, two servers) and the fleets: which gateway has the stateless ACK/RST bypass (`bypass`), which offloads (`offload`) |
| `scenarios/traffic.py` | what a flow does: `tcp_short`, `tcp_talk`, `udp_rr`, `udp_stream` |
| `scenarios/steady.py` | a scenario: the flows as a grid of traffic x client x server x forward gateway x return gateway |
| `expect.py` | what "works" means, for any implementation |
| `impl/*.py` | one file per implementation: `none`, `flowsync`, `flowsync_conntrack`, `conntrackd` |

A flow is named `traffic:client>forward gateway>server>return gateway`.

**The expectation** (`expect.py`) has no fixed allowance for loss. Every lab
first measures how fast the implementation syncs, from outside: new flows
over every gateway pair whose server holds its first answer back by 0 to
1000 ms. From that and each path:

- One gateway: clean from the first packet.
- Two gateways, and the answer's detour to the server and back is at least
  the sync latency: the race can be won, clean from the first packet.
- Two gateways and a shorter detour: what the client sends in the first
  (sync latency - detour) of a flow may go unanswered, and the endpoint's own
  retry has to repair it: the first UDP request after that time, the first
  TCP SYN retransmission (1, 3, 7 s) after it.
- An event (reroute, state loss, blackout, uplink re-created): how fast the
  implementation repairs what it broke is a number in the report (recovery:
  from the end of the event to the last packet lost), not a verdict. Judged
  is that the flow delivers again before it ends.
- A reset or a stalled connection is never accepted. If no answer passed in
  the measurement, nothing is accepted on a path through two gateways.

Results are pass or FAIL; the measured sync latency is in the report's
header, how many flows needed a retry is a column.

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

## Time

Every timer is written in its production value: the gateways' conntrack
settings in `timers.py`, an implementation's own timers in its adapter (as it
ships them), a scenario's durations and event times in its traffic. One
factor divides them all: `--scale N`, default 10 (1 = production time). So
what expires before what stays as on a gateway, and a run takes minutes.

- Link delays, the sync itself and the endpoints' TCP retransmission timers
  are real time and do not scale. The larger the scale, the slower a real
  second of sync latency is against the timers: at 100 it weighs like 100 s.
  High scales are fair only to implementations that sync in milliseconds.
- No timer goes below a second (the kernel takes conntrack timeouts in whole
  seconds, the daemons their options). 10 is the highest scale at which every
  timer divides evenly. Above it the run warns and lists what is held at 1 s
  or rounded.
- Beyond some scale a run tests nothing production does, and is refused with
  the reason and the highest usable scale. In general that is 80: above it a
  half-open TCP connection (120 s) is forgotten before the client's first SYN
  retry (1 s, real time). An implementation can add its own: flowsync_conntrack's limit
  is 39, where its refresh (element_timeout + interval) stops being shorter
  than the UDP stream timeout, which the daemon itself warns about.
- `longlived` runs at scale 30: 4.5 hours of connection in nine minutes, with
  only the TCP close timeout (10 s) held at 1 s.

## Scenarios

    python3 -m gwlab SCENARIO --impl NAME [--scale N]     # a module in scenarios/

| Scenario | What happens | Result |
|---|---|---|
| `steady` | 400 flows over every gateway pair, three fleets | pass / FAIL per flow |
| `reroute` | a third in every return path moves to the next gateway, two thirds in every forward path | pass / FAIL |
| `stateloss` | gw2 loses every flow it knows (the implementation's `lose_state`) | pass / FAIL |
| `blackout` | gw2 is cut off from the sync while every flow starts | pass / FAIL |
| `uplink` | gw2's uplink device is deleted and created again (new ifindex, same MAC) | pass / FAIL |
| `symasym` | flows start on one gateway, are split over two and end on one again, twice: egress first, then ingress first. | pass / FAIL |
| `restart` | the sync software on gw2 is stopped and started again | pass / FAIL |
| `idle` | connections fall silent for ten minutes, then the server speaks; ten more, then the client | pass / FAIL |
| `syncloss` | gw2's sync path loses 30 % of its packets, all the time | pass / FAIL against the sync latency measured under that loss |
| `syncmtu` | gw2's sync path drops packets above 1280 bytes | pass / FAIL |
| `longlived` | one connection per gateway pair talks for 2.2 established timeouts, scale 30 | pass / FAIL |
| `latency` | new flows to a server at distance 0 that holds its first answer back by 0 to 1000 ms | from which delay on every answer passes: the sync latency, seen from outside |
| `perf` | one flow at a time, nothing else running, no link latency | numbers, below |

A bad sync path (`sync=dict(gw2=dict(loss=30))` or `mtu=1280` in the
topology) is there from the start, also while the sync latency is measured:
the flows are judged against what the implementation achieves on it. The
answers held back in that measurement stay below the lifetime of an unanswered
UDP flow; a later answer would not pass a single gateway either.

After an event a flow has to deliver again and no connection may reset; the
report's recovery column says how long the loss went on after the event
ended (median / worst over the flows that lost something, in real seconds).
A loss later in the flow that has nothing to do with the event shows up
there as a long recovery. A repair that rides on a timer takes scale times
longer on a router.

`perf` is a measurement (`ALONE = True`: labs and flows one after the other)
on three gateways, each traffic over one gateway and over two:

| Traffic | Result |
|---|---|
| `flood_up`, `flood_down` | packets per second through the path, and the kernel's ns per packet on the gateway that forwarded them |
| `bulk` | Mbit/s of one TCP download |
| `newflows` | new flows per second (500 to 12000) whose answer, 50 ms later, still gets through |

Next to each: CPU time of the implementation's processes (user / system)
plus the kernel's packet path, for the forward gateway, the return gateway
and a gateway that only receives the sync. The kernel number comes from
giving each gateway interface a receive thread of its own (threaded NAPI)
and reading the threads' CPU time: firewall, connection tracking, tc
programs and forwarding of that gateway, whatever the implementation.

The numbers compare implementations; they are not a router's. Compare only
runs from the same environment (all in the VM, or all on the host) on an
otherwise idle machine: the same kernel path measured 579 and 747 ns per
packet in two runs on a loaded host.
