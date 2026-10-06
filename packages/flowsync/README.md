# flowsync

Flow tables for the Freifunk Berlin active-active IPv6 gateways, without
conntrack. Replaces conntrackd (Mode NOTRACK) + samplicator.

Outbound traffic of a mesh client may leave through gateway A while the reply
enters through gateway B. Both run a stateful firewall towards the mesh: a
packet from the Internet is forwarded only if it belongs to a flow a client
started. With flowsync that state is not a conntrack entry:

- **Forwarded IPv6 bypasses conntrack** (`notrack`). conntrack stays in charge
  of what the gateway itself sends and receives, and of IPv4.
- **Two tc programs on the uplink device** keep and consult two BPF maps.
  *Egress*: every forwarded IPv6 packet leaving through the uplink keeps its
  flow alive in the **local** map. *Ingress*: a packet from the uplink whose
  flow is alive in the local or the **remote** map gets a packet mark.
- **The firewall accepts the mark** (one rule in fw4's forward chain) and
  treats everything else from the uplink as before: the stateless rules, then
  reject.
- **The daemon** announces the local flows to the other gateways and writes
  what they announce into the remote map.

Only the uplink is looked at. Traffic that reaches a gateway through a tunnel
from another gateway passed that gateway's uplink, and was checked there.

This is a prototype on its own branch. It was tested on x86_64 with kernel 7.3
in a VM (see "Tests"); it has not run on OpenWrt or on the gateways' hardware
yet, and the OpenWrt package build is untested (see "Open points").

## Design

- **No shared state.** The local map is written by the egress program only,
  the remote map by the daemon only. There is no entry both sides write, so
  there is no ownership to track, nothing to tell apart and nothing that can
  keep itself alive: only the local map is announced, and only packets write
  it.
- **Soft state.** An announcement means "this flow exists now". A peer holds
  it for `element_timeout` after the last announcement; every flow is
  announced when it starts and again every `interval`. No sequence numbers,
  acks or close messages. A lost datagram is repaired by the next round, and a
  gateway that may have missed announcements asks its peers for a round at
  once (see "Liveness and resync").
- **The client keeps a flow alive, nobody else.** Only packets going out
  refresh a flow. Packets from the server do not: a server cannot hold its own
  way in open once the client has fallen silent.
- **Every forwarded flow is local state, a configured subset is synced.** The
  local map holds every flow the gateway forwarded out of its uplink,
  whatever its addresses and protocol (it replaces conntrack for them). What
  is announced to the peers and accepted from them is decided by the policy
  below.
- **The datapath outlives the daemon.** The maps are pinned in the BPF file
  system and the programs stay attached when the daemon stops. Flows on a
  symmetric path keep working, peers' flows until they expire. A restart
  loads the programs anew, reuses the maps and replaces the filters in place.
- **Fail closed.** Without the programs nothing is marked, and everything
  from the uplink meets the stateless rules and the reject, as unsolicited
  traffic does.
- **One thread, no blocking.** New-flow events, datagrams and the map walks
  are driven from one `poll()` loop.

### What is a flow

The key is client address, server address, protocol, client port, server
port. TCP, UDP and SCTP have ports; every other protocol (ESP, GRE, IP in IP,
L2TP, ...) is one flow per pair of addresses. ICMPv6 has no flows: the
firewall handles it statelessly (fw4's `Allow-ICMPv6-Forward`), errors about
a flow included.

Protocols by what clients actually use across a gateway:

| | handled as | synced by default |
|---|---|---|
| TCP, UDP (with QUIC, WireGuard, WebRTC, ... on top) | ports | yes |
| ESP: IPsec without UDP encapsulation, as phones (WiFi calling) and VPN clients send it where there is no NAT | address pair | yes |
| GRE, IPv4 in IPv6, IPv6 in IPv6, L2TPv3: plain tunnels | address pair | yes |
| SCTP: hardly seen natively (WebRTC carries it inside UDP) | ports | no (`proto sctp`) |
| anything else | address pair | no (`proto <number>`) |
| ICMPv6 | no flow, stateless in the firewall | - |

There is nothing like conntrack's helpers (FTP, SIP, H.323): they read
unencrypted control connections to let in a second connection the outside
starts. The gateways have never loaded any, and what is in use today either
starts every flow from the client or is encrypted.

A local flow lives for a timeout after its last packet out:

| | option | default |
|---|---|---|
| UDP | `udp_timeout` | 180 s |
| TCP, only SYNs seen so far | `tcp_syn_timeout` | 120 s |
| TCP, after any other segment from the client | `tcp_timeout` | 7440 s |
| TCP, after the client's FIN or RST | `tcp_close_timeout` | 120 s |
| other protocols | `other_timeout` | 600 s |

That is all the TCP state there is: two flags per flow, set from the client's
segments (a new SYN on the tuple clears them). There is no window tracking and
nothing is learned from the server's segments, which on an asymmetric path
the forwarding gateway does not see anyway. A segment in the middle of a
connection (after a reboot, or when the path moved here) makes an established
flow.

### Fragments and extension headers

tc programs see packets before the kernel's reassembly and have to find the
transport header themselves.

- The programs walk hop-by-hop, routing, destination-options, AH and fragment
  headers (up to 6) to the transport header.
- Only the first fragment of a packet has the ports. *Egress* refreshes the
  flow from it and ignores the others. *Ingress* marks the first fragment
  only, and the kernel's reassembly, which runs before the firewall's forward
  chain, builds the reassembled packet on the first fragment's packet header:
  the packet the firewall sees carries its mark. That is how
  `inet_frag_reasm_prepare()` works (`skb_morph(skb, head)`), not a documented
  interface; the tests `frag_sym`, `frag_asym` and `frag_unsolicited` hold it
  down.
- Reassembly is hooked in as long as any rule in the ruleset uses conntrack
  (fw4's input chain does). The daemon's own table contains such a rule in a
  chain that is never run, so that this does not depend on the firewall.

### The rules

Two things in nftables, both kept in place by the daemon (it is told when the
ruleset changes, and looks every `interval`):

1. **Table `inet flowsync`**, the daemon's own, untouched by `fw4 reload`:

       chain prerouting {
           type filter hook prerouting priority raw; policy accept;
           meta nfproto ipv6 fib daddr type unicast notrack
       }

   `unicast` is what will be forwarded: routed, not one of the gateway's
   addresses, not multicast. At the first start it is installed one
   `interval` after the programs were attached: by then the flows that were
   running under conntrack have sent a packet and are in the local map, and
   until then conntrack still accepts their replies (test `bootstrap`).

2. **One rule in fw4's `forward` chain**:

       meta nfproto ipv6 meta mark & 0x01000000 == 0x01000000 accept comment "flowsync"

   An accept in the daemon's own table would not do: fw4's chain still sees
   the packet and rejects it. The package ships the rule as an fw4 include
   (`/usr/share/nftables.d/chain-pre/forward/10-flowsync.nft`), so it is part
   of every ruleset fw4 loads. If it is missing all the same, the daemon
   inserts it (`fw_repaired`, and a warning the second time).

   Its place in the chain matters. bbb-configs clamps the MSS of every
   forwarded IPv6 SYN with a rule it prepends to the same chain, and a
   SYN/ACK from the uplink must pass that rule before it is accepted on its
   mark. fw4 renders the includes from its own configuration (the clamp)
   before the ones it finds in `/usr/share/nftables.d` (this rule), and the
   daemon inserts a missing rule right before fw4's `ct state` rule, behind
   every prepended include, not at the top (tests `mss_clamp`, `rules`).

The ingress program clears the mark bit on every IPv6 packet from the uplink
before it decides, so nothing can bring the mark in from outside.

With conntrack out of the forward path, fw4's `ct state established,related`
rule no longer matches forwarded IPv6. What the gateways' ruleset does besides
that is stateless already (zone forwardings, `inbound_allow`, the ICMPv6
rule, the MSS clamp, the ACK/RST budget) and works as before.

### Bypass

Optional (`bypass`, off by default). With it the ingress program does not
only mark an accepted packet, it forwards it: route lookup, hop limit minus
one, and out through the neighbour layer of the outgoing device
(`bpf_fib_lookup`, `bpf_redirect_neigh`). Such a packet passes neither
nftables, nor conntrack's hooks, nor the kernel's IPv6 forwarding path. It
covers the direction from the uplink to the mesh only, which is where the
programs sit and where most bytes go.

Bypassed is only what needs none of the above; everything else keeps its mark
and takes the normal path:

| not bypassed | because |
|---|---|
| packets without a flow | the firewall decides: stateless rules, reject |
| anything but TCP and UDP directly behind the IPv6 header | fragments must be reassembled before the firewall; extension headers and other protocols are rare |
| TCP segments with SYN, FIN or RST | the firewall clamps the MSS on SYNs |
| hop limit 0 or 1 | the stack sends the ICMPv6 error |
| too big for the outgoing device (GSO segments included) | the stack sends "packet too big" |
| no route, or the route leads back out of the uplink or to the gateway itself | not ours to forward here |

What it costs:

- **A change of the firewall's rules does not reach flows that are in the
  tables.** A rule added to block a destination applies to new flows and to
  the packets on the normal path; packets of a running flow keep bypassing it
  until the flow ends. `flowsync bypass off` puts everything back through the
  firewall at once.
- **nftables counters, tracing and tcpdump on the uplink's ingress see the
  packets, the forward chain does not.** `dp_in_bypass` counts them.
- **The size check is against the outgoing device's MTU**, not a route or
  path MTU below it.
- **The uplink must be an Ethernet-like device** (the daemon checks and leaves
  the bypass off otherwise): the helper takes a link-layer header off the
  packet. The outgoing device may be anything with a neighbour layer; tunnel
  devices have not been tried.
- The egress side of the outgoing device (its qdisc and tc filters) is passed
  as usual.

The switch is a map entry the programs read per packet, so it can be changed
while they run: `flowsync bypass on|off` (or `/etc/init.d/flowsync bypass
...`), which lasts until the daemon attaches the programs again and applies
its configuration.

Measured in the same VM as the other numbers (pktgen on one CPU, 10k flows,
from the uplink to the client): about 0.72 µs per packet with the bypass,
0.79 µs with conntrack, 0.96 µs with the tables and no bypass, 0.64 µs with
an empty ruleset. The other direction is unchanged.

### Filter policy

Which flows are synced; applied identically on TX and RX:

- IPv6, and the protocol is in `proto` (default `udp`, `tcp`, `esp`, `gre`,
  `ipip`, `ip6ip6`, `l2tp`); never ICMPv6
- client is inside at least one `prefix` and inside no `exclude`
- server is inside no `exclude_dst` (the mesh prefix, no default)
- for UDP, the server port is not in `skip_server_port` (default `53`)
- TCP, UDP and SCTP: both ports are 1..65535; every other protocol: no ports
- both addresses routable (not unspecified, loopback, multicast, link-local
  or v4-mapped)

A flow that does not pass is still a local flow on the gateway that forwards
it: its replies pass there and nowhere else (test `policy`).

A spoofed sender can therefore at most create entries for (client inside our
prefixes) x (server outside the mesh), each living `element_timeout` seconds.
An entry of a protocol without ports lets everything of that protocol from
that server to that client in, not one port.

### TX

- The egress program reports the first packet of every flow through a ring
  buffer; a wanted one is announced to all peers at once. In the tests a
  reply that takes 1 ms to come back already finds the flow on the reply
  gateway (`race_window`).
- Every `interval` the local map is walked (512 entries per system call, a
  few batches per loop iteration, only while the send queue has room). An
  entry past its timeout is deleted, every other wanted one is announced
  again, at `tx_rate` datagrams per second per peer. Then the remote map is
  walked for what has expired. A round that is not sent when the next
  `interval` comes is not restarted and nothing is dropped; the next round
  starts as soon as it is through (`refresh_overrun`, raise `tx_rate`).
- Events that did not fit the ring (`ev_overruns`), and announcements a full
  send buffer could not take, are repaired by a round pulled forward to one
  second from then, at most once per interval.

### RX

- UDP socket on `bind_address`:`port`, bound to the `interface` device and
  reopened when that device is created anew (netifd does that to a VLAN on
  every `ifup`). Datagrams from addresses not listed as `peer` are dropped,
  and so are IPv6 datagrams whose source is a v4-mapped address.
- Every record that passes the policy is written to the remote map, alive for
  `element_timeout` from now; one system call per burst of datagrams. An
  announcement of a flow that is there sets its time anew.
- The remote map holds `max_copies` flows. When it is full, new flows are
  refused (`rx_limited`) while the ones it has are still refreshed.

### Wire format

Unchanged from the conntrack version. Binary UDP; one datagram is a 4 byte
header followed by `count` records of 40 bytes, at most 34 records per
datagram, by default 30 (1252 bytes as an IPv6/UDP packet):

    header:  version(1)=1  count(1)  flags(1)  reserved(1)=0
    record:  proto(1)  flags(1)=0  cport(2)  sport(2)  reserved(2)=0
             client(16)  server(16)

A datagram with `count` 0 is a heartbeat; header flag `0x01` asks the
receiver for a round now (resync).

### Liveness and resync

- Every `interval` each daemon sends a heartbeat to all peers. The status
  file shows per peer when the last datagram came, and a peer silent for
  three intervals is logged.
- A gateway asks its peers for a round at startup (after a reboot the remote
  map is empty; after a restart the announcements from the downtime are
  missing) and when its sync socket had to be reopened. A peer answers with
  a round at `resync_rate` (default four times `tx_rate`), at most once per
  requesting peer per `interval`/4 and twice per `interval` overall. In the
  tests a rebooted gateway holds its peers' flows again within 1.5 s
  (`reboot`).

## Configuration

The daemon is configured on the command line only. The init script renders
`/etc/config/flowsync` (written by bbb-configs) into these options.

    flowsync [options] <command>
      run                                        run the daemon
      check                                      print the parsed configuration
      status                                     print counters, peers, packet counters
      announce <client> <cport> <server> <sport> [proto] send one record to all peers
      flows [local|remote]                       list the flow tables
      flow <client> <cport> <server> <sport> [proto] look one flow up in both tables
      bypass [on|off]                            show or switch the bypass of the running programs
      detach                                     take the programs, maps and rules away

| option | UCI option | default | meaning |
|---|---|---|---|
| `-b, --bind ADDR` | `bind_address` | any | local address of the sync socket; peers check the source address, so set it to the address the peers list |
| `-I, --interface DEV` | `interface` | any | sync datagrams are accepted only when they arrive on this device. The UCI option may name the logical interface, the init script passes its device |
| `-U, --uplink DEV` | `uplink` | `interface` | the device forwarded traffic leaves to the Internet on; the tc programs attach there. Waited for if it does not exist yet, followed when it is created anew |
| `--bypass` | `bypass` | off | forward accepted TCP and UDP packets from the uplink's tc hook, past netfilter (see "Bypass") |
| `-p, --port N` | `port` | `3780` | UDP port, the same on all gateways |
| `-i, --interval SEC` | `interval` | `30` | seconds between rounds and counter logs |
| `-t, --element-timeout SEC` | `element_timeout` | `90` | lifetime of a peer's flow after its last announcement; at least `3 x interval` |
| `-l, --batch-lines N` | `batch_lines` | `30` | records per datagram, 1..34 |
| `-r, --tx-rate N` | `tx_rate` | `500` | refresh datagrams per second, per peer |
| `-R, --resync-rate N` | `resync_rate` | `0` | the same for a round that answers a resync request; `0`: four times `tx_rate` |
| `-B, --rcvbuf BYTES` | `rcvbuf` | `8388608` | receive buffer of the sync socket |
| `-m, --mark HEX` | `mark` | `0x01000000` | packet mark of accepted packets; the fw4 include must name the same value |
| `-F, --max-flows N` | `max_flows` | `131072` | size of the local map. It is an LRU: when full, the flow that has been idle longest makes room. About 100 bytes per entry, allocated at start |
| `-C, --max-copies N` | `max_copies` | `131072` | size of the remote map; when full, new flows are refused |
| `--udp-timeout SEC` etc. | `udp_timeout`, `tcp_timeout`, `tcp_syn_timeout`, `tcp_close_timeout`, `other_timeout` | see above | lifetime of a local flow after its last packet out |
| `-P, --proto NAME` | `proto` (list) | `udp`, `tcp`, `esp`, `gre`, `ipip`, `ip6ip6`, `l2tp` | synced protocols: these names, `sctp`, or a protocol number. Giving the option replaces the default list |
| `-S, --skip-server-port N` | `skip_server_port` (list) | `53` | UDP server ports never synced |
| `-e, --peer ADDR` | `peer` (list) | - | the other gateways (max. 32) |
| `-x, --prefix CIDR` | `prefix` (list) | - | synced client prefixes |
| `-X, --exclude CIDR` | `exclude` (list) | - | client prefixes not synced |
| `-D, --exclude-dst CIDR` | `exclude_dst` (list) | - | server prefixes not synced |
| `-s, --status-file PATH` | - | `/var/run/flowsync/status` | where `run` writes its counters |
| `-u, --user NAME` | - | - | run as this user with `CAP_NET_ADMIN` only once everything is open |
| `--bpf-object PATH` | - | `/lib/bpf/flowsync.o` | the tc programs |
| `--pin-dir PATH` | - | `/sys/fs/bpf/flowsync` | where the maps are pinned |
| `--fw-table NAME` | - | `fw4` | the firewall's `inet` table |
| `-d, --debug` | `debug` | off | log every record sent and received |

Changing `max_flows` or `max_copies` makes new maps at the next start: the
flows in the old ones are dropped (local flows are learned again from their
next packet out, the peers' by the resync). The timeouts and the mark are
part of the programs and take effect with a restart, the maps stay.

### Privileges

The daemon starts as root, loads the programs, attaches them and opens its
sockets, then drops every capability but `CAP_NET_ADMIN` and, with `--user`,
switches to that user. `CAP_NET_ADMIN` is what attaching the programs again,
reopening the sync socket and `nft` need; the maps and the event ring are
used through descriptors that are open by then (on kernel 6.12 and later that
needs no `CAP_BPF`).

## Operation

`/etc/init.d/flowsync status|check|flows|flow|announce|bypass|detach`.

Stopping the service leaves the programs on the uplink and the maps in place.
`/etc/init.d/flowsync detach` takes everything away (programs, maps, the
table, the rule): forwarded IPv6 is conntrack's again from the next packet,
and flows that were running have no conntrack entry until their client sends
again.

The status file (`flowsync status`), written every `interval`:

| | meaning |
|---|---|
| `attached` | the programs are on the uplink |
| `bypass` | the bypass is on |
| `fw_ok` | both rules are in place |
| `local` / `copies` | live flows in the local / remote map at the last round |
| `tx_events` | flows announced from their first packet |
| `tx_refresh` | records announced by the rounds |
| `ev_recv` / `ev_overruns` | new-flow events read / lost because the ring was full |
| `refresh_rounds`, `refresh_overrun`, `refresh_errors`, `refresh_ms`, `refresh_entries` | the rounds |
| `rx_records`, `rx_policy`, `rx_bad_peer`, `rx_parse`, `rx_version` | what came from the peers |
| `rx_limited` / `rx_errors` | flows the full remote map refused / other map errors |
| `local_expired` / `remote_expired` | entries the rounds removed |
| `dp_attached` | how often the programs were attached (1, plus one per re-created uplink or lost filter) |
| `fw_repaired` | how often the accept rule had to be inserted |
| `dp_out_pkts`, `dp_out_new`, `dp_out_skip` | the egress program: packets looked at, first of a flow, without a flow (ICMPv6, later fragments) |
| `dp_in_pkts`, `dp_in_local`, `dp_in_remote`, `dp_in_miss`, `dp_in_skip` | the ingress program: looked at, accepted on a local / a peer's flow, no flow, not parsed |
| `dp_in_bypass` | of the accepted packets, forwarded by the program itself |
| `loop_max_ms` | longest time the loop spent between two polls in the last interval |

and one line per peer, `peer <address> rx <datagrams> age <seconds> tx_errors <n>`.

## Known limits

- **A reply can be faster than the announcement.** It then meets the stateless
  rules: for TCP the ACK/RST budget, which carries it; without that rule it is
  rejected and the client's SYN retransmission finds the flow in place (test
  `tcp_race`). In the tests a reply that takes 1 ms is never faster. Keep the
  ACK/RST budget for this and for the seconds after a reboot.
- **No TCP state on the reply side.** Any segment with the 5-tuple passes
  while the flow lives. The conntrack version's copies were liberal too.
- **Closed connections linger.** The reply gateway learns nothing from the
  server's FIN or RST, and a client that vanishes without a FIN leaves its
  flow for `tcp_timeout`. The local map is an LRU, so a full map evicts the
  idlest flows first, but an evicted flow that was merely idle loses its way
  back in.
- **An idle connection lives `tcp_timeout`** after the client's last segment,
  then the server's next push is reset (test `tcp_idle_limit`). conntrack's
  limit on an asymmetric path was `tcp_timeout_unacknowledged` (300 s).
- **Sync is unauthenticated**, as before, and a gateway that masquerades mesh
  traffic to its uplink address lets mesh hosts send to the peers' sync port
  from a peer address (see the conntrack version's review, G-02): only the
  firewall can stop that.
- **`max_copies` refuses in arrival order**; there is no per-client limit
  (the conntrack version had `max_copies_per_client`).
- **Flow offloading** needs conntrack and cannot be used for forwarded IPv6
  on a gateway that runs this.
- **The first start cuts flows that stay silent**: a flow that sends no
  packet out during the first `interval` is not in the local map when the
  notrack table is installed, and its replies are rejected until its client
  sends again.
- Adding or removing a gateway requires re-rendering all gateways.

## Open points

Not done or not verified in this prototype:

- The OpenWrt package Makefile follows `bridger` (BPF toolchain, `/lib/bpf`)
  but has not been built: no BPF toolchain was available here.
- Nothing has run on kernel 6.12, on a big-endian target or on the
  edgerouter-4. The per-packet cost of the programs is unmeasured.
- The mark a reassembled packet inherits is kernel behaviour read from the
  6.18 source and tested on 7.3.
- The notrack rule covers all forwarded IPv6, also between mesh interfaces;
  bbb-configs' own `NOTRACK` rules become redundant, and its ruleset should be
  read once more for anything that still expects conntrack state there.
- The bypass has forwarded to veth devices only. Forwarding into GRE or
  WireGuard devices, which is where the gateways' mesh traffic goes, is
  untested, and so is its gain on the gateways' hardware.
- A device that is both uplink and carries tunnel traffic sees the tunnel's
  outer packets only; inner packets are looked at where they leave an uplink.

## Source layout and tests

`src/`, plain C on libbpf; the tc programs in `src/bpf/flowsync.bpf.c`.

| file | content |
|---|---|
| `dp.h` | map keys and values, shared by the programs and the daemon |
| `bpf/flowsync.bpf.c` | the egress and ingress programs |
| `dp.c` | loading, attaching, pinned maps, events, map walks |
| `fw.c` | the two nftables rules, kept in place |
| `tx.c` | new-flow events and the rounds |
| `rx.c` | datagram handling |
| `resync.c` | heartbeats, resync requests, peer liveness |
| `policy.c`, `config.c`, `wire.c`, `udp.c`, `status.c`, `util.c`, `main.c` | as named |
| `test_flowsync.c` | unit test: prefixes, policy, wire format, options, lifetimes |

`test/` holds the integration tests, documented in
[test/README.md](test/README.md): the real daemon and programs on two or
three gateways in network namespaces, with forwarded traffic between a client
and a server namespace. They need root, because BPF programs cannot be loaded
from a user namespace.

    make -C src test                       # unit test
    sudo make -C src itest                 # integration tests, default matrix (~1.5 min)
    sudo make -C src itest S="asym frag_asym"
