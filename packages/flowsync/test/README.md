# flowsync integration tests

`fstest` runs the real daemon and its tc programs on several gateways in
network namespaces and checks what reaches the endpoints, what the firewall
rejects and what is in each gateway's flow tables. Python 3 stdlib only; needs
iproute2 (with `tc`), nft and util-linux.

**It needs root.** BPF programs cannot be loaded from a user namespace, so
every worker runs under `unshare -n` as real root; each gateway pins its maps
in a directory of its own under `/sys/fs/bpf` (`fstest-<pid>-g<i>`). A
throwaway VM on the host's kernel works well for that (virtme-ng:
`vng -r --user root -- make -C src itest`), and is how the suite was
developed.

    make -C src itest                          # default matrix, quiet (~1.5 min)
    make -C src itest S="asym frag_asym"       # some scenarios
    make -C src itest P="g3 plain noack" S=tcp_race   # one combination
    make -C src itest M=quick                  # production-like combination only
    make -C src itest T=tcp                    # by tag
    make -C src itest S=tcp_idle_limit         # tagged slow, not in the default run
    make -C src itest J=8 V=1 K=1              # jobs, print every run, keep all logs
    make -C src check                          # unit test + default matrix
    tail -f /tmp/flowsync-test.log             # every check, timestamped (LOG=...)
    cd test && python3 -m fstest --list        # scenarios and matrices

The output is the failures, a scenario x combination table and a summary;
the exit status is 1 on any failure. Logs of failed runs stay in the
directory the summary names: per scenario `checks.log` (every check) and
`g<i>.log` (the daemon's debug log, with its counters every round).
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
          mesh0 fd00:i:1::1 \    |    / wan0 fd00:i:2::1   <- the uplink: tc programs
                             \   |   /
  CL: m<i> fd00:i:1::c        (one link per gateway)        SV: w<i> fd00:i:2::5
  client addresses on lo                                 server addresses on lo
  2001:db8:100:<flow>::1                              2a00:1450:4001:<flow>::e
```

- Each gateway routes the client prefix `2001:db8:100::/44` via mesh0 and the
  server net via wan0. Its ruleset is fw4's in small: the accept rule for
  marked packets (as the package's include puts it there), `ct state
  established,related accept`, `iifname mesh0 accept`, per profile the
  rate-limited stateless accept for TCP segments with ACK or RST, reject.
  Counters `fwd_mark`, `fwd_est`, `fwd_ack`, `fwd_rej` tell which rule took a
  packet. The daemon's own table (`notrack` for forwarded IPv6) is there from
  the start, so conntrack never accepts a reply in the daemon's place; the
  scenario `bootstrap` removes it to test the first start.
- The sync device (eth0, IPv4) and the uplink (wan0) are different devices
  here (`-I eth0 -U wan0`); on the gateways they are the same.
- Every flow gets its own client and server address. CL routes the server
  /128 via the flow's forward gateway, SV routes the client /128 via its
  reverse gateway, so each direction takes the gateway the test chooses, and
  `flow.reroute()` moves it. `via=g` sends a single packet through another
  gateway (SO_MARK i+1, policy-routing table 100+i on CL and SV).
- CL and SV count each flow's packets that were forwarded (hop limit 63).
  Raw-packet flows are dropped there, so the endpoint kernels never answer
  them; `real=True` flows (sockets) pass. A fragmented datagram counts once
  per fragment, so the fragment scenarios use real sockets and look at what
  the application received.
- Scaled timers: interval 3 s, element_timeout 9 s, local flows UDP 12 s,
  TCP 40 s (SYN only 10 s, closing 8 s), other protocols 15 s.
- Between scenarios a worker stops the daemons, runs `flowsync detach` on
  every gateway (programs, maps and rules gone) and loads the rulesets anew.

## Profiles and matrices

A combination is a gateway count and, per gateway, whether it has the
stateless ACK/RST rule: `g3 plain ack`, `g3 plain noack`, `g2 plain ack`
(the default matrix). Only scenarios tagged `tcp` depend on the rule; the
others run once per gateway count (`==` in the table). The `offload` tokens
of the spec syntax are still parsed but mean nothing here: flow offloading
needs conntrack.

## Scenarios

`python3 -m fstest --list` prints them; each one's docstring says what it
shows.

| module | scenarios |
|---|---|
| `basic.py` | `sym`, `unsolicited`, `asym`, `latency`, `expiry`, `server_cannot_hold`, `long_flow`, `reroute`, `policy`, `tunnel_protos`, `proto_list`, `other_proto`, `idle_peers` |
| `frag.py` | `frag_sym`, `frag_asym`, `frag_unsolicited`, `exthdr` |
| `tcp.py` | `tcp_state`, `tcp_pickup`, `tcp_connect`, `tcp_race`, `race_window`, `tcp_idle_push`, `tcp_idle_limit` (slow), `tcp_busy` |
| `ops.py` | `daemon_down`, `restart_keeps`, `reboot`, `uplink_recreate`, `sync_recreate`, `rules`, `bootstrap`, `lost_first`, `loss`, `forged`, `unprivileged` |
| `bypass.py` | `bypass`, `bypass_default_off`, `bypass_normal_path`, `bypass_tcp` |
| `load.py` | `scale` (heavy; `SCALE_FLOWS`, default 50000) |

`FSTEST_BYPASS=1` in the environment runs every daemon of every scenario with
`--bypass`; the whole matrix passes that way too.

## Writing a scenario

    @scenario(gateways=2, tags={"tcp"})
    def name(env):
        """what it shows"""
        g0, g1 = env.g[:2]
        env.start()                             # daemons on all gateways
        f = env.flow("udp", fw=g0, rev=g1)      # out via g0, back via g1
        f.send()                                # one packet client -> server
        synced(env, f)                          # the other gateways hold it
        env.check("replies pass g1 (of 3)", replies(env, f), 3)

- `g.ft(f)` is the flow in g's tables (`.local`, `.remote`, `.local_left`,
  `.remote_left`, `.flags`); `g.flows("local"|"remote")` lists a table;
  `g.st(key)` reads a counter from the status file (written at the daemon's
  tick, `g.tick()` waits for the next one); `g.fwc(name)` a firewall counter.
- `env.check`, `env.wait_for`, `env.hold` never raise: a run reports all its
  failures.
- Tools for traffic are in `test/probe.py` (UDP of any size, raw TCP
  segments, real TCP clients and servers, extension headers, other
  protocols, floods).

## Known limits of the suite

- `latency` measures with a query that itself takes tens of milliseconds
  (a process per look); `race_window` is the sharper measurement.
- Everything runs on one machine over veth links: no real MTUs, no
  reordering, and the gateways' CPUs are the host's.
