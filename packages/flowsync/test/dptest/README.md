# dptest: flowsync's datapath, from inside a gateway

[gwlab](../gwlab/README.md) answers whether a fleet of gateways delivers, as
the endpoints see it, for any implementation. dptest is the other layer: what
is specific to this one and cannot be judged from the endpoints.

| Looked at here | Scenarios |
|---|---|
| what the flow tables hold, accept and expire; nothing passes without a flow | `sym`, `asym`, `unsolicited`, `expiry`, `server_cannot_hold`, `idle_peers` |
| which flows are synced: policy, protocols without ports | `policy`, `tunnel_protos`, `proto_list`, `other_proto` |
| what the tc programs parse themselves: fragments, extension headers, TCP flags | `frag_sym`, `frag_asym`, `frag_unsolicited`, `exthdr`, `tcp_state`, `tcp_pickup` |
| the bypass, and what it must leave to the firewall and to policy routing | `bypass`, `bypass_default_off`, `bypass_normal_path`, `bypass_tcp`, `bypass_fwmark`, `bypass_otherhost`, `bypass_gre`, `bypass_wg` |
| the accept rule: behind the MSS clamp, with the daemon's mark, looked at on changes only | `mss_clamp`, `mss_clamp_bypass`, `rules`, `rules_idle`, `mark_mismatch` |
| the datapath without its daemon, a reboot, a re-created uplink, rules and programs put back, the first start | `daemon_down`, `restart_keeps`, `reboot`, `uplink_recreate`, `rules`, `bootstrap` |
| what fails: programs that cannot be attached, a daemon that cannot load them, a table of another size | `fail_open`, `load_failure`, `resize` |
| limits, forged announcements, running without root's capabilities | `forged`, `unprivileged` |
| a burst of new flows (slow, not in the default run) | `scale` |

Whether real connections survive latency, reroutes, loss on the sync path or
a restart is gwlab's, and is not repeated here.

    make -C src dptest                       # all scenarios (~1.5 min)
    make -C src dptest DS="frag_asym bypass" # some
    make -C src dptest DPTEST_BYPASS=1       # every daemon with --bypass
    make -C src dptest DS=scale              # tagged slow
    cd test && python3 -m dptest --list

**It needs root.** BPF programs cannot be loaded from a user namespace. As
root `make dptest` runs here (workers under `unshare -n`, each gateway's maps
pinned in a directory of its own under `/sys/fs/bpf`); otherwise it runs in a
throwaway VM on the host's kernel (`dptest/vm.sh`, virtme-ng, `VNG=` if `vng`
is not in PATH). The output is the failures, a table and a summary; the exit
status is 1 on any failure. Per scenario there is `checks.log` (every check)
and `g<i>.log` (the daemon's debug log) in the directory the summary names
(in the VM: `OUT`, default `/tmp/dptest-out`).

## Topology

```
                     hub: br0 10.0.0.250/24 (sync bridge)
                  s0 |          s1 |          s2 |
               eth0 10.0.0.1  eth0 10.0.0.2  eth0 10.0.0.3
                 +------+      +------+      +------+
                 |  g0  |      |  g1  |      |  g2  |   flowsync on each, port 3994
                 +------+      +------+      +------+
          mesh0 fd00:i:1::1 \    |    / wan0 fd00:i:2::1   <- the uplink: tc programs
                             \   |   /
  CL: m<i> fd00:i:1::c        (one link per gateway)        SV: w<i> fd00:i:2::5
  client addresses on lo                                 server addresses on lo
  2001:db8:100:<flow>::1                              2a00:1450:4001:<flow>::e
```

- No link latency: a reply can be faster than the announcement here, and the
  scenarios that need the flow on the other gateway wait for it (`synced`).
- Each gateway's ruleset is fw4's in small: bbb-configs' MSS clamp on every
  forwarded SYN, the accept rule for marked packets (as the package's include
  puts it there), `ct state established,related accept`, `iifname mesh0
  accept`, reject. No stateless ACK/RST accept: nothing may pass without a
  flow. Counters `fwd_mark`, `fwd_est`, `fwd_rej` tell which rule took a
  packet. The daemon's own table is there from the start, without the alive
  element, which the daemon writes; `bootstrap` removes the table to test the
  first start, `dead_uplink` leaves it without a daemon.
- The sync device (eth0) and the uplink (wan0) are different devices here
  (`-I eth0 -U wan0`); on the gateways, and in gwlab, they are the same.
- Every flow has its own client and server address; CL routes the server via
  the flow's forward gateway, SV the client via its reverse gateway.
  `flow.reroute()` moves either; `via=g` sends one packet through another
  gateway.
- CL and SV count each flow's packets that were forwarded (hop limit 63).
  Raw-packet flows are dropped there, so the endpoint kernels never answer
  them; `real=True` flows (sockets) pass. A fragmented datagram counts once
  per fragment, so the fragment scenarios use sockets and look at what the
  application received.
- Scaled timers: interval 3 s, element_timeout 9 s, local flows UDP 12 s,
  TCP 40 s (SYN only 10 s, closing 8 s), other protocols 15 s.
- `bypass_gre` and `bypass_wg` put a tunnel over the mesh link of a gateway,
  IPv4 outer, as the gateways have toward each other (GRE, MTU 1476) and from
  the corerouters (WireGuard, MTU 1280; set up with pyroute2, no `wg` needed),
  and route a flow's client through it.
- Between scenarios a worker stops the daemons, runs `flowsync detach` on
  every gateway (programs, maps and rules gone) and loads the rulesets anew.

## Writing a scenario

    @scenario(gateways=2)
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
  tick, `g.tick()` waits for the next one); `g.fwc(name)` a firewall counter;
  `g.cmd(...)` runs a flowsync subcommand on the gateway's maps.
- `env.check`, `env.wait_for`, `env.hold` never raise: a run reports all its
  failures.
- Traffic tools are in `probe.py` (UDP of any size, raw TCP segments, TCP
  clients and servers, extension headers, other protocols, floods).
