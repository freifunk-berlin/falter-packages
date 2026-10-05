# flowsync integration tests

`fstest` runs the real daemon on several gateways in unprivileged network
namespaces (no root: every worker runs under its own `unshare -Urn`) and checks
what ends up in each gateway's conntrack table and what reaches the endpoints.
Python 3 stdlib only; needs iproute2, nft and util-linux.

    make -C src itest                                 # default matrix, quiet (~3 min)
    make -C src itest S="resync tcp_idle"             # some scenarios
    make -C src itest P="g3 off=01 ack=-" S=tcp_idle  # one combination, named as printed
    make -C src itest M=quick                         # production-like combination only
    make -C src itest M=full                          # 2..4 gateways, 33 combinations
    make -C src itest T=tcp                           # by tag
    make -C src itest S=scale P="g3 plain ack"        # 100k flows (tagged slow)
    make -C src itest J=8 V=1 K=1                     # jobs, print every run, keep all logs
    make -C src check                                 # unit test + default matrix
    tail -f /tmp/flowsync-test.log                    # every check, timestamped (LOG=...)
    cd test && python3 -m fstest --list               # scenarios and matrices

The output is the failures, a scenario x combination table and a summary;
the exit status is 1 on any failure. Logs of failed runs stay in the
directory the summary names: per scenario `checks.log` (every check) and
`g<i>.log` (the daemon's debug log, with its status line every round).
Machine-specific build flags (headers or libraries outside the system paths)
go into `src/local.mk`, which the Makefile includes and git ignores.

## Topology

```
                     hub: br0 10.0.0.250/24 (sync bridge)
                  s0 |          s1 |          s2 |
               eth0 10.0.0.1  eth0 10.0.0.2  eth0 10.0.0.3
                 +------+      +------+      +------+
                 |  g0  |      |  g1  |      |  g2  |   flowsync on each, port 3780
                 +------+      +------+      +------+
          mesh0 fd00:i:1::1 \    |    / wan0 fd00:i:2::1
                             \   |   /
  CL: m<i> fd00:i:1::c        (one link per gateway)        SV: w<i> fd00:i:2::5
  client addresses on lo                                 server addresses on lo
  2001:db8:100:<flow>::1                              2a00:1450:4001:<flow>::e
```

- Each gateway routes the client prefix `2001:db8:100::/44` via mesh0 and the
  server net via wan0, and has an fw4-like forward chain: policy drop,
  `ct state established,related accept`, `iifname mesh0 accept`, reject; plus
  per profile `flow offload @ft` (flowtable on mesh0 and wan0) and the
  rate-limited stateless accept for TCP segments with ACK or RST.
- Every flow gets its own client and server address. CL routes the server
  /128 via the flow's forward gateway, SV routes the client /128 via its
  reverse gateway, so each direction takes the gateway the test chooses, and
  `flow.reroute()` moves it. `via=g` sends a single packet through another
  gateway (SO_MARK i+1, policy-routing table 100+i on CL and SV).
- CL and SV count each flow's packets that were forwarded (hop limit 63):
  delivery is measured at the endpoints, not with gateway counters, which
  offloaded packets bypass. Raw-packet flows are dropped there, so the
  endpoint kernels never answer them; `real=True` flows (sockets) pass.
- Scaled timers: interval 3 s, element_timeout 9 s, UDP 10/20 s, TCP
  syn_sent 12 s, unacknowledged 300 s, flowtable 3 s. Scenarios that need
  other values set them and the reset restores them.

## Architecture

| file | content |
|---|---|
| `fstest/ns.py` | `Node`: a network namespace held by `unshare -n sleep infinity`; run, spawn, nft, sysctl |
| `fstest/topo.py` | `Topology`: hub, gateways, CL, SV, links, routes; `reset()` between scenarios |
| `fstest/gateway.py` | `Gateway`: profile, ruleset, daemon start/stop/restart, status counters, conntrack queries (`ct(flow)` -> `Entry`), sync blocking/loss/MTU, `hold()` of a flow's server packets |
| `fstest/flow.py` | `Flow` (addresses, paths, `send`, raw `tcp`, `delivered`), `Bulk` (thousands of flows) |
| `fstest/env.py` | `Env`: what a scenario gets: `g`, `cl`, `sv`, `flow()`, `bulk()`, `start()`, `spawn()`, checks, timers |
| `fstest/check.py` | `check` / `true` / `wait_for` / `hold`: one ok/FAIL line each, a failed check never aborts the scenario |
| `fstest/matrix.py` | profile specs and the named matrices |
| `fstest/scenario.py` | the `@scenario` registry |
| `fstest/runner.py` | parent: matrix expansion, workers, table; child: one worker's scenarios |
| `fstest/selftest.py` | unit test of the framework (specs, matrices, dedup) |
| `probe.py` | packet and socket tool run inside the namespaces (send, flood, raw TCP, echo/TCP servers and clients, spoof) |
| `ctquery.c` | conntrack get/count/flush through the daemon's own ctnetlink code |
| `ptyrun.py` | runs the daemon on a pty so it logs line by line |

**Profiles and combinations.** A profile is per gateway: flow offloading
on/off, ACK rule on/off. A combination is a gateway count plus a profile per
gateway, written `g3 off=01 ack=012` (offloading on g0 and g1, ACK rule on
all; `-` for none). Specs also take `plain`, `offload`, `mixed` (all but the
last offload), `ack`, `noack` and `g1:offload,noack`. `default`: g3 plain ack
(production-like), g3 offload ack, g3 mixed ack, g3 plain noack, g3 mixed
noack, g2 plain ack.

**Scenarios** are functions declared with
`@scenario(gateways=N, requires=need(pred, reason), tags={...}, once=False, depends=None)`.
`requires` skips combinations it cannot run in (shown `--`), `once` runs a
profile-independent scenario in the first combination only. A scenario
depends on offloading, and on the ACK rule only if tagged `tcp`; a
combination equal to an earlier one in what the scenario depends on is not
run again (shown `==`). Checks are semantic and hold with and without
offloading: `Entry.is_copy` (our mark), `is_native`, `carries_traffic(E)`
(ASSURED, and offloaded or a timeout above a refresh's), `offloaded`,
`tcp_state` (None while offloaded: the dump has no protocol info then).

**Workers.** Each combination's runs are spread, longest first, over
workers in proportion to their total duration (durations of earlier runs are
cached in `~/.cache/flowsync-fstest-durations.json`). A worker builds its
topology once and resets it between scenarios (daemons stopped, tables
flushed, flows, servers and rules removed, counters and timers restored); after
a failure it rebuilds. Scenarios tagged `heavy` run one after the other in a
worker of their own, and alone: every run holds a load lock, shared, and a
heavy one exclusive, so no flood runs beside another scenario's timing checks.
A scenario whose daemon exits on its own fails, and so does one whose
background traffic generator fails. Default `--jobs`: two thirds of the CPUs,
at most 24.

## Scenarios

g = minimum gateway count. "Copy" is an entry flowsync injected (marked),
"native" one the kernel created from a packet.

### Basic (`scenarios/basic.py`)

- **basic** (g3): one flow forward via g0, reply via g1. Copies appear on g1
  and g2, g1's turns ASSURED from the reply, a copy with traffic announces
  itself, g0's native is never touched by announcements.
- **dead_flow** (g3): the flow stops. g0's native expires on its own timeout,
  the copies soon after, announcements stop: nothing keeps a dead flow alive.
- **reroute** (g3): the forward path moves from g0 to g1 mid-flow. g1's copy
  announces itself from traffic, g2 keeps its copy, g0 gets the flow back as
  a copy; all gone after the traffic stops.
- **two_origins** (g3): g0 and g2 both forward the same flow and both hold it
  natively; neither announcement touches the other's native.
- **loss** (g3): g1 loses every third record datagram (heartbeats exempt):
  element_timeout = 3 intervals, so one lost refresh never costs a copy.
- **sparse_udp** (g3): a UDP flow on a copy with a packet period between
  element_timeout and the stream timeout: the reply gateway's copy is there at
  every reply.
- **mtu** (g2): sync packets above 1280 bytes are lost on the path: the
  default datagram (30 records) fits.

### TCP (`scenarios/tcp.py`, raw segments unless noted)

- **tcp_handshake** (g3): SYN and client data via g0, SYN/ACK and server data
  via g1 on the injected ESTABLISHED copy. g0 stays SYN_SENT (the client's ACK
  is invalid there and forwarded anyway), g1's copy turns ASSURED and is
  announced, g0 gets the flow back as a copy once its SYN_SENT expires.
- **tcp_pickup** (g2): mid-stream data without a handshake is picked up as
  ESTABLISHED (tcp_loose) and synced.
- **tcp_rst** (g2): the server resets; the copy goes to CLOSE, a refresh does
  not revive it, it is gone once the origin stops announcing.
- **tcp_race** (g2): the SYN/ACK beats the announcement (sync to g1 blocked):
  it is invalid on g1 and passes only through the ACK rule; the next refresh
  brings the copy and the retransmission passes as established.
- **tcp_idle** (g2): an idle asymmetric connection keeps its state on the
  reply gateway: the copy that saw server traffic is not cut back by
  refreshes, and the server may speak first after g0's entry expired.
- **reversed** (g2, needs the ACK rule on g1): a server segment that reaches g1
  before the copy is picked up as a connection server -> client; the next
  announcement replaces it with the copy.
- **reversed_real** (g2, ACK rule on g1): the same with real TCP stacks.
- **dns_tcp** (g2): `skip_server_port 53` applies to UDP only; DNS over TCP is
  synced.
- **tcp_flush_norule** (g2, no ACK rule on g1): a real connection survives
  `conntrack -F` on the reply gateway: the resync restores the copy before the
  server's next segment (without the rule a gap means a reset).

### Restarts and resync (`scenarios/restart.py`)

- **restart** (g2): g1's daemon restarts under a live flow; the copy survives
  in the kernel, ownership is re-learned from the mark, refreshes resume.
- **flush** (g2): `conntrack -F` on g1; the DESTROY events trigger a resync
  request and the copy is back at once.
- **expiry_resume** (g2, once, interval 10 s): a flow idles until g1's copy
  has expired, then resumes on the same 5-tuple; g0's announcement from the
  NEW event must create the copy at once, not a round or two later.
- **restart_idle_copy** (g3): after a restart a copy seen for the first time
  is no evidence: an idle copy is not announced again.
- **stale_own** (g2): g1's copy is flushed and g1 then forwards the flow
  itself; g0's announcement must not touch g1's new native (EXCL create).
- **resync** (g2, interval 10 s): after a daemon restart the copy is
  refreshed within seconds (the restarted daemon asks its peers for a round),
  after a reboot (empty table) the copy is back within 3 s.
- **peer_liveness** (g3): a dead peer shows in the status (last heard) and is
  logged after three silent intervals, as is its return.

### Firewall and offload (`scenarios/firewall.py`, real sockets)

- **fw_udp** (g2): without g1's daemon the reply is rejected by g1's forward
  chain; with it the reply passes on the copy and the copy announces itself.
- **fw_tcp** (g3): 1 MiB each way over the asymmetric path; g1 holds the
  server's packets until the copy is there (so the outcome does not depend on
  which comes first), then only the copy lets them pass.
- **offload** (g3, offloading on g1): a copy carrying a flow in both
  directions is offloaded; being offloaded is the evidence: g1 announces it
  every round, g2 keeps its copy, all gone after the traffic.
- **offload_native** (g3, offloading on g0): a symmetric flow offloaded from
  its second packet never gets ASSURED; it is announced from the fourth dump
  phase and the peers keep their copies.

### Load (`scenarios/load.py`, heavy, first combination only)

- **enobufs**: 2000 new flows overrun the event socket; no resubscribe, a
  round repairs the gap, every flow reaches g1.
- **enobufs_early**: flows lost to an overrun are announced by an early round,
  not the next regular one.
- **overrun**: tx_rate too low, a round takes longer than the interval: the
  next is delayed, nothing dropped.
- **stuck_dump**: a round held back by its own send queue is slow, not stuck.
- **flush_retry**: `conntrack -F` on g1 with 20000 copies: all back within 3 s
  (resync), not at g0's next round.
- **flush_stale_chunk**: `conntrack -F` on g1 while its copies phase is held
  back by its own send queue (3000 copies with traffic, tx_rate 6): the dump
  chunk generated before the flush is read after the DESTROY events and must
  not make g1 take the gone copies for its own; every copy is back with g0's
  next round once sync works, none stays missing.
- **scale** (tagged slow, not in the default run): 100000 flows through the
  streaming dump, the event socket and the per-tuple table.

### Abuse and configuration (`scenarios/abuse.py`)

- **garbage** (g2): garbage from a peer, a datagram from a non-peer, a record
  outside the policy: counted, nothing injected.
- **spoof** (g2, heavy, once): a flood of policy-conforming forged records
  from a peer's address; the copy limit protects the table and a legitimate
  copy survives.
- **noise** (g2, once): IPv4, unconfigured protocols and foreign clients never
  reach user space; with more than 20 prefixes the check moves to user space.
- **default_tcp** (g1, once): the default configuration syncs UDP and TCP.

## Known issues

- **Timing under host load.** Checks wait for things to happen within a few
  intervals. Kernel resources are shared by all namespaces (the per-CPU
  receive backlog, the conntrack hash, the rtnl lock), so a flood in one
  worker can stall daemons (`loop_max_ms` of seconds) or drop packets in
  others. The heavy scenarios therefore run alone (load lock), and more jobs
  than about two thirds of the CPUs make checks fail. A failure that does not
  reproduce with `P=... S=...` alone is most likely load; the daemon log's
  `refresh_ms` / `loop_max_ms` show it.
- **Profile coverage of load and abuse scenarios.** They run in the first
  combination of the run only (plain, with the ACK rule in `default`); there
  is no flood test with offloading.
- **Host kernel vs. production.** The tests run on the host's kernel, not on
  OpenWrt's 6.12; flowtable and ctnetlink details (offload GC, which flows get
  offloaded) differ between versions. Offloaded entries dump without timeout
  and TCP state until the flowtable tears them down; checks wait for that.
- **Userns limits.** Socket buffers are capped at `rmem_max` (warnings in the
  daemon log), `nf_conntrack_max` is global and read-only; the flowtable timeout
  sysctl may not be writable (then 30 s is assumed).
- **Gateway-local traffic** appears only in `noise`, which tests the event
  filter; everything else is forwarded traffic.
- **Uninitialised memory.** The host's gcc build may leave zeros where code
  reads bytes nobody wrote (struct padding compared byte for byte once made
  the reversed-entry replacement depend on stack contents: it passed here and
  could fail on the router). A build that fills every uninitialised local with
  a pattern turns such bugs into failures; run the matrix against it:

      make -C src clean
      make -C src CC=clang CFLAGS="-O2 -g -ftrivial-auto-var-init=pattern" all
      make -C src itest

  (gcc's `-ftrivial-auto-var-init=pattern` did not fill that variable, clang's
  does; add the include paths of `src/local.mk` to `CFLAGS` if you use it.)
- **Remaining daemon race** (see the main README): a packet arriving in the
  milliseconds between the dump reading a copy and its refresh can have its
  timeout raise undone by the refresh.
