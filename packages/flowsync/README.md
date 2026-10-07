# flowsync

Flow tables for the Freifunk Berlin active-active IPv6 gateways, without
conntrack. Replaces conntrackd (Mode NOTRACK) + samplicator.

Outbound traffic of a mesh client may leave through gateway A while the reply
enters through gateway B. Both run a stateful firewall towards the mesh: a
packet from the Internet is forwarded only if it belongs to a flow a client
started. With flowsync that state is not a conntrack entry:

- **Forwarded IPv6 bypasses conntrack** (`notrack`) while the programs are on
  the uplink. conntrack stays in charge of what the gateway itself sends and
  receives, and of IPv4.
- **Two tc programs on the uplink device** keep and consult two BPF maps.
  *Egress*: every forwarded IPv6 packet leaving through the uplink keeps its
  flow alive in the **local** map. *Ingress*: a packet from the uplink whose
  flow is alive in the local or the **remote** map gets a packet mark. They
  sit on the device's tcx hooks (kernel 6.6 and later), not on a qdisc: SQM
  or any other qdisc and filter setup on the uplink stays as it is.
- **The firewall accepts the mark** (one rule in fw4's forward chain) and
  treats everything else from the uplink as before: the stateless rules, then
  reject.
- **The daemon** announces the local flows to the other gateways and writes
  what they announce into the remote map.

Only the uplink is looked at. Traffic that reaches a gateway through a tunnel
from another gateway passed that gateway's uplink, and was checked there.

It was tested on x86_64 with kernel 7.3 in a VM (see "Source layout and
tests"); it has not run on OpenWrt or on the gateways' hardware yet, and the
OpenWrt package build is untested (see "Open points").

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
  loads the programs anew, reuses the maps and replaces the programs in place.
- **No qdisc of its own.** The programs attach to the uplink's tcx hooks
  (`BPF_TCX_INGRESS`, `BPF_TCX_EGRESS`), the place a clsact qdisc's filters
  run at, without the qdisc. See "Next to SQM and other tc users".
- **No flow, no way in.** What the programs do not mark meets the stateless
  rules and the reject, as unsolicited traffic does.
- **Without the programs, back to conntrack, by itself.** With forwarded
  IPv6 untracked, only the programs can accept a reply. So what leaves
  through the uplink is untracked only while its interface index is in a
  set with a timeout, `alive_timeout` (10 s), which the daemon refreshes every
  third of that, each time after seeing both programs on the uplink, and
  deletes the moment it finds them gone; what comes in is untracked on the
  mark. An expired element tracks, it never rejects: without the daemon, or
  without the programs, conntrack carries the flows on a symmetric path
  within `alive_timeout`, as it did before flowsync, and the asymmetric ones
  wait. Flows conntrack carried keep passing on their entries after the
  programs are back, until they are in the map. No interval, no learning
  phase, nothing a human has to end.
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
  interface; the dptest scenarios `frag_sym`, `frag_asym` and
  `frag_unsolicited` hold it down.
- Reassembly is hooked in as long as any rule in the ruleset uses conntrack
  (fw4's input chain does). The daemon's own table contains such a rule in a
  chain that is never run, so that this does not depend on the firewall.

### The rules

Two things in nftables, both kept in place by the daemon (it is told when the
ruleset changes, and looks every `interval`):

1. **Table `ip6 flowsync`**, the daemon's own, untouched by `fw4 reload`:

       set alive {
           type iface_index
           flags timeout
       }
       chain prerouting {
           type filter hook prerouting priority raw; policy accept;
           meta mark & 0x01000000 == 0x01000000 notrack accept
           fib daddr oif @alive notrack
       }

   A packet the ingress program marked is untracked: its flow is in the
   tables. `accept` ends this chain only (conntrack and fw4 still run); it
   spares every accepted packet the route lookup of the second rule, which
   cannot match it. A packet routed out of an interface in `alive` is
   untracked: that is the uplink, while the daemon vouches for it. The daemon
   writes the element over netlink (no fork) every `alive_timeout`/3 seconds,
   as a destroy and an add with `timeout` and `expires` in one transaction
   (only kernel 6.12 and later refresh an element that exists; the destroy
   is no error on a missing one), right after `dp_tick()` found both
   programs on the uplink; it deletes it at once when they are not there.
   The chain's place is fixed: after defragmentation (-400, which skips
   untracked packets) and before conntrack (-200); `raw` is the only named
   priority in that window. Without a
   refresh the element is gone after `alive_timeout`, and what leaves is
   tracked: conntrack has an entry for the flow when an unmarked reply comes
   back. Family `ip6`, so the rules need no `meta nfproto`; the device's
   index rather than its name, so the lookup compares four bytes. The daemon
   installs the table at start if it is missing, in one transaction that
   replaces whatever was there (`add table`, `delete table`, the definition),
   with the element included when the programs are attached. Its own
   element writes carry its netlink port id and do not count as a ruleset
   change; what it writes through `nft` does, and costs one look.

2. **One rule in fw4's `forward` chain**:

       meta nfproto ipv6 meta mark & 0x01000000 == 0x01000000 accept comment "flowsync"

   An accept in the daemon's own table would not do: fw4's chain still sees
   the packet and rejects it. The package ships the rule as an fw4 include
   (`/usr/share/nftables.d/chain-pre/forward/10-flowsync.nft`), so it is part
   of every ruleset fw4 loads. If it is missing all the same, the daemon
   inserts it (`fw_repaired`, and a warning the second time). A rule with
   the comment and another mark than the daemon's (`mark` changed, the
   include not) is replaced, with a warning: every firewall reload brings
   the include's rule back, and replies are rejected for the moment until
   the daemon has replaced it again. Write the same mark into both.

   Its place in the chain matters. bbb-configs clamps the MSS of every
   forwarded IPv6 SYN with a rule it prepends to the same chain, and a
   SYN/ACK from the uplink must pass that rule before it is accepted on its
   mark. fw4 renders the includes from its own configuration (the clamp)
   before the ones it finds in `/usr/share/nftables.d` (this rule), and the
   daemon inserts a missing rule right before fw4's `ct state` rule, behind
   every prepended include, not at the top.

The ingress program clears the mark bit on every IPv6 packet from the uplink
before it decides, so nothing can bring the mark in from outside.

With conntrack out of the forward path, fw4's `ct state established,related`
rule no longer matches forwarded IPv6 to or from the uplink; it is what
carries the symmetric flows whenever the element is absent. Forwarding
between mesh interfaces is tracked as before flowsync. What the gateways'
ruleset does besides that is stateless already (zone forwardings,
`inbound_allow`, the ICMPv6 rule, the MSS clamp, the ACK/RST budget) and
works as before; `drop_invalid` must stay off, since conntrack sees only one
direction of a flow it tracks while the programs come and go.

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
| frames addressed to another host's MAC (a NIC in promiscuous mode receives them) | the stack drops them; tc sees them first |
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
- **The route is looked up with the mark the packet has at the uplink's tc
  hook** (the daemon's included), as on the normal path. Routing rules that
  match a mark which nftables sets later, in prerouting, do not see it: such
  a setup needs the bypass off. bbb-configs has none.
- **The uplink must be an Ethernet-like device** (the daemon checks and leaves
  the bypass off otherwise): the helper takes a link-layer header off the
  packet. The outgoing device may be anything with a neighbour layer: veth,
  an IPv4 GRE tunnel and a WireGuard device have been tried (dptest
  `bypass_gre`, `bypass_wg`), which is what the gateways' mesh traffic goes
  into.
- The egress side of the outgoing device (its qdisc and tc filters) is passed
  as usual.
- **A bypassed packet skips the uplink's ingress shaping.** The program runs
  before the device's tc filters, so SQM's redirect to its IFB never sees
  what the program forwarded itself; only the packets that take the normal
  path (new flows, SYNs, fragments, ...) are shaped on the way in. The
  shaping of what leaves through the uplink is untouched. A gateway that
  shapes its downstream must run with the bypass off, or accept that only
  the uplink's own queue limits the flows in the tables.

The switch is a map entry the programs read per packet, so it can be changed
while they run: `flowsync bypass on|off` (or `/etc/init.d/flowsync bypass
...`), which lasts until the daemon attaches the programs again and applies
its configuration.

Measured in the same VM as the other numbers (pktgen on one CPU, 10k flows,
from the uplink to the client): about 0.72 µs per packet with the bypass,
0.79 µs with conntrack, 0.96 µs with the tables and no bypass, 0.64 µs with
an empty ruleset. The other direction is unchanged.

### Next to SQM and other tc users

The programs are attached to the uplink's tcx hooks, not to a qdisc. A tcx
hook is where a clsact qdisc's filters run, before the device's tc filters
and independent of them: an `ingress` qdisc with SQM's redirect to an IFB,
cake or fq_codel as the root qdisc, or a clsact of another tool can be on the
uplink at the same time, added before or after the daemon attached, and
removed or re-added under the programs without the daemon noticing. Nothing
of the scheduler has to be configured, and the package depends on no
scheduler module (the kernel's `CONFIG_NET_XGRESS`, which BPF selects).

What the two see of each other:

- The ingress program marks a packet before SQM's filter redirects it to the
  IFB; the mark travels with the packet. A packet the IFB hands back skips
  the ingress hook (`tc_skip_classify`), so the program runs once per packet.
- The egress program runs before the root qdisc: everything leaving is kept
  in the local map, whatever the shaper does with it afterwards.
- With the bypass on, bypassed packets never reach the ingress qdisc's
  filters, so the downstream shaping does not apply to them (see "Bypass").
- A previous daemon's programs are replaced in place at startup; programs of
  others on the same hooks are left alone, ours are appended behind them.
  Attaching and detaching tcx programs sends no netlink notification: if
  something detaches the programs, the daemon notices at its next tick (up to
  `interval`), a re-created device at once.

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
it: its replies pass there and nowhere else.

A spoofed sender can therefore at most create entries for (client inside our
prefixes) x (server outside the mesh), each living `element_timeout` seconds.
An entry of a protocol without ports lets everything of that protocol from
that server to that client in, not one port.

### TX

- The egress program reports the first packet of every flow through a ring
  buffer; a wanted one is announced to all peers at once.
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
  every `ifup`). Datagrams from addresses not listed as `peer` are dropped.
  IPv4 peers are matched as v4-mapped addresses; an IPv6 datagram whose
  source is a v4-mapped address is dropped too, no peer sends one.
- Every record that passes the policy is written to the remote map, alive for
  `element_timeout` from now; one system call per burst of datagrams. An
  announcement of a flow that is there sets its time anew.
- The remote map holds `max_remote` flows. When it is full, new flows are
  refused (`rx_limited`) while the ones it has are still refreshed.

### Wire format

Binary UDP; one datagram is a 4 byte
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
  tests a rebooted gateway holds its peers' flows again within 1.5 s.
- A peer that comes back is asked too. Every peer sends at least its
  heartbeat per `interval`, so a datagram from a peer whose last one is more
  than one and a half intervals old means the path was down or lost that
  heartbeat, and with it the announcements made meanwhile: the receiver asks
  that peer for a round at once (once per peer per `interval`/2) instead of
  waiting for its next regular one. Both sides of a partition see the other
  come back, so both ask. The repair starts with the first datagram after
  the gap, which on a busy gateway is the next new flow; on a quiet one it
  is the next heartbeat, so a partition shorter than an interval on a quiet
  path is repaired by the regular round only.

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
| `-b, --bind ADDR` | `bind_address` | any | local address of the sync socket, IPv4 or IPv6 (the socket is dual-stack); peers check the source address, so set it to the address the peers list |
| `-I, --interface DEV` | `interface` | any | sync datagrams are accepted only when they arrive on this device. The UCI option may name the logical interface, the init script passes its device |
| `-U, --uplink DEV` | `uplink` | `interface` | the device forwarded traffic leaves to the Internet on; the tc programs attach there. Waited for if it does not exist yet, followed when it is created anew |
| `--bypass` | `bypass` | off | forward accepted TCP and UDP packets from the uplink's tc hook, past netfilter (see "Bypass") |
| `-p, --port N` | `port` | `3994` | UDP port, the same on all gateways (IANA-unassigned; conntrackd's 3780 is not) |
| `-i, --interval SEC` | `interval` | `30` | seconds between rounds and counter logs |
| `-t, --element-timeout SEC` | `element_timeout` | `90` | lifetime of a peer's flow after its last announcement; at least `3 x interval` |
| `-l, --batch-lines N` | `batch_lines` | `30` | records per datagram, 1..34 |
| `-r, --tx-rate N` | `tx_rate` | `500` | refresh datagrams per second, per peer |
| `-R, --resync-rate N` | `resync_rate` | `0` | the same for a round that answers a resync request; `0`: four times `tx_rate` |
| `-B, --rcvbuf BYTES` | `rcvbuf` | `8388608` | receive buffer of the sync socket |
| `-A, --alive-timeout SEC` | `alive_timeout` | `10` | how long what leaves through the uplink stays untracked after the daemon last saw the programs on it; refreshed every third of it. Expiry tracks, it never rejects |
| `-m, --mark HEX` | `mark` | `0x01000000` | packet mark of accepted packets; the fw4 include must name the same value |
| `-F, --max-flows N` | `max_flows` | `131072` | size of the local map. It is an LRU: when full, the flow that has been idle longest makes room. About 100 bytes per entry, allocated at start |
| `-C, --max-remote N` | `max_remote` | `131072` | size of the remote map; when full, new flows are refused. Preallocated like the local map (a map that allocates per entry refills its caches from the timer tick on MIPS and refuses a resync's burst) |
| `--udp-timeout SEC` etc. | `udp_timeout`, `tcp_timeout`, `tcp_syn_timeout`, `tcp_close_timeout`, `other_timeout` | see above | lifetime of a local flow after its last packet out |
| `-P, --proto NAME` | `proto` (list) | `udp`, `tcp`, `esp`, `gre`, `ipip`, `ip6ip6`, `l2tp` | synced protocols: these names, `sctp`, or a protocol number. Giving the option replaces the default list |
| `-S, --skip-server-port N` | `skip_server_port` (list) | `53` | UDP server ports never synced |
| `-e, --peer ADDR` | `peer` (list) | - | the other gateways (max. 32), IPv4 or IPv6, all of one family, the same as `bind_address` |
| `-x, --prefix CIDR` | `prefix` (list) | - | synced client prefixes |
| `-X, --exclude CIDR` | `exclude` (list) | - | client prefixes not synced |
| `-D, --exclude-dst CIDR` | `exclude_dst` (list) | - | server prefixes not synced |
| `-s, --status-file PATH` | - | `/var/run/flowsync/status` | where `run` writes its counters |
| `-u, --user NAME` | - | - | run as this user with `CAP_NET_ADMIN` only once everything is open |
| `--bpf-object PATH` | - | `/lib/bpf/flowsync.o` | the tc programs |
| `--pin-dir PATH` | - | `/sys/fs/bpf/flowsync` | where the maps are pinned |
| `--fw-table NAME` | - | `fw4` | the firewall's `inet` table |
| `-d, --debug` | `debug` | off | log every record sent and received |

Changing `max_flows` or `max_remote` makes that map anew at the next start:
the flows in the old one are dropped (local flows are learned again from
their next packet out, the peers' by the resync), the other map stays. The
timeouts and the mark are part of the programs and take effect with a
restart, the maps stay. A start that fails (no object file, programs the
kernel refuses) leaves the maps and the programs of the last run alone.

### Privileges

The daemon starts as root, loads the programs, attaches them and opens its
sockets, then drops every capability but `CAP_NET_ADMIN` and, with `--user`,
switches to that user. `CAP_NET_ADMIN` is what attaching the programs again,
reopening the sync socket and `nft` need; the maps and the event ring are
used through descriptors that are open by then (on kernel 6.12 and later that
needs no `CAP_BPF`). Replacing a previous daemon's programs in place takes a
descriptor of them (`BPF_PROG_GET_FD_BY_ID`, `CAP_SYS_ADMIN`), which only the
start, as root, needs: a device the daemon attaches to later has none of ours
on it.

## Operation

`/etc/init.d/flowsync status|check|flows|flow|announce|bypass|detach`.

Stopping the service leaves the programs on the uplink and the maps in place.
`/etc/init.d/flowsync detach` takes everything away (programs, maps, the
table, the rule): forwarded IPv6 is conntrack's again from the next packet,
and flows that were running have no conntrack entry until their client sends
again. Removing the package does the same first. Upgrading it does not: the
package restarts the service, the new daemon loads the new programs and
replaces the old ones in place, the maps and the alive element stay.

A service that stays stopped keeps what it left, but not the switch: the
alive element expires after `alive_timeout`, and from then on what leaves
through the uplink is tracked, so conntrack carries the symmetric flows and
the asymmetric ones wait for the daemon. A device created anew has a new
index, so it is tracked from its first packet. Nothing is rejected for
longer than `alive_timeout`, and nothing needs a human. Stop it for good
with `detach`.

The rules are looked at when the ruleset changes (a netlink notification),
when the programs come or go, and every `interval` only while something is
not in place: a gateway in order runs `nft` after a firewall reload and
otherwise not at all.

The status file (`flowsync status`), written every `interval`:

| | meaning |
|---|---|
| `attached` | the programs are on the uplink |
| `jited` | the programs run JIT-compiled (0: interpreted, see "Open points") |
| `bypass` | the bypass is on |
| `fw_ok` | the accept rule and the table are in place |
| `alive` | the uplink's element is in the set: what leaves through it is untracked (0 while conntrack carries) |
| `local` / `remote` | live flows in the local / remote map at the last round |
| `tx_events` | flows announced from their first packet |
| `tx_refresh` | records announced by the rounds |
| `ev_recv` / `ev_overruns` | new-flow events read / lost because the ring was full |
| `refresh_rounds`, `refresh_overrun`, `refresh_errors`, `refresh_ms`, `refresh_entries` | the rounds |
| `rx_records`, `rx_policy`, `rx_bad_peer`, `rx_parse`, `rx_version` | what came from the peers |
| `rx_limited` / `rx_errors` | flows the full remote map refused / other map errors |
| `local_expired` / `remote_expired` | entries the rounds removed |
| `dp_attached` | how often the programs were attached (1, plus one per re-created uplink or lost filter) |
| `fw_repaired` | how often the accept rule had to be inserted or replaced |
| `dp_out_pkts`, `dp_out_new`, `dp_out_skip` | the egress program: packets looked at, first of a flow, without a flow (ICMPv6, later fragments) |
| `dp_in_pkts`, `dp_in_local`, `dp_in_remote`, `dp_in_miss`, `dp_in_skip` | the ingress program: looked at, accepted on a local / a peer's flow, no flow, not parsed |
| `dp_in_bypass` | of the accepted packets, forwarded by the program itself |
| `loop_max_ms` | longest time the loop spent between two polls in the last interval |

and one line per peer, `peer <address> rx <datagrams> age <seconds> tx_errors <n>`.

## Migration from conntrackd

flowsync (UDP port 3994) and conntrackd with samplicator (3780) cannot read
each other's datagrams, and need not share a port: both can be installed and
running on a gateway during the flip. While the fleet is mixed, every asymmetric flow
whose forward and reply gateway run different software has no state on the
reply gateway: with k of n gateways migrated that is 2k(n-k)/(n(n-1)) of
those flows, 33 % for one of six, 60 % for three of six. So no canary and no
rollout over days:

- **One window:** build all images in one change and flash the gateways back
  to back (`sysupgrade` reboots, so there is nothing to clean up), or in two
  halves within minutes.
- **Or a scripted flip:** ship an image with both packages, conntrackd active
  and flowsync with `enabled '0'`; then stop and disable conntrackd and
  samplicator and start flowsync on all gateways within seconds.

On a gateway that carries traffic the first start hands forwarded IPv6 over
from conntrack flow by flow: a flow is untracked from its next packet out,
and until then conntrack accepts its replies on the entry it has (see "The
rules"). Nothing is cut. Going back is `/etc/init.d/flowsync detach`
(conntrack tracks forwarded IPv6 again from the next packet; flows that were
running have no entry until their client sends) and starting conntrackd.

Which addresses: the peers' uplink addresses, IPv4 or IPv6, and
`bind_address` the gateway's own. IPv6 needs a route from the uplink address
to every peer; a source-specific default route for the mesh prefix alone
does not give one when the uplink address lies outside that prefix, and an
address inside it on `lo` is reached over the mesh, not the `interface`
device. The uplink IPv4 addresses, as conntrackd used them, are the safe
choice; they need an input rule for flowsync's port from the peers'
addresses on the uplink zone, conntrackd's rule goes with it afterwards. The
firewall must also drop forwarded UDP to the peers' sync port from the mesh
zone, or any mesh host is a peer (see "Known limits").

bbb-configs has to render the options this version has (`uplink` where it is
not the sync interface, `mark` if the default bit is taken, the timeouts if
they should differ) and to stop rendering conntrackd's. Keep the stateless
ACK/RST accept; the firewall rule for the sync port is a new one, for
flowsync's port. The conntrack-based
flowsync, which was never deployed, is on branch `flowsync-conntrack`.

## Known limits

- **A reply can be faster than the announcement.** It then meets the stateless
  rules: for TCP the ACK/RST budget, which carries it; without that rule it is
  rejected and the client's SYN retransmission finds the flow in place. How
  long an announcement takes is measured from outside by gwlab (`latency`).
  Keep the ACK/RST budget for this and for the seconds after a reboot.
- **No TCP state on the reply side.** Any segment with the 5-tuple passes
  while the flow lives.
- **Closed connections linger.** The reply gateway learns nothing from the
  server's FIN or RST, and a client that vanishes without a FIN leaves its
  flow for `tcp_timeout`. The local map is an LRU, so a full map evicts the
  flows idle longest, where idle means no packet in either direction (the
  ingress lookup counts as use too), in the kernel's approximate order; an
  evicted flow that was merely idle loses its way back in.
- **An idle connection lives `tcp_timeout`** after the client's last segment,
  then the server's next push is reset.
- **Sync is unauthenticated.** Peers are recognised by their source address
  on the `interface` device. A gateway that masquerades mesh traffic to its
  uplink address (bbb-configs does, for IPv4) rewrites a mesh host's datagram
  to exactly that address and sends it out of the uplink: any mesh host can
  then send announcements to every gateway but its own exit. Only the
  firewall can stop that: no forwarding to the peers' sync port (see
  "Migration from conntrackd").
- **`max_remote` refuses in arrival order**; there is no limit per client.
- **Flow offloading** needs conntrack and cannot be used for forwarded IPv6
  on a gateway that runs this.
- **The outbound notrack rule decides by destination alone.** `fib daddr oif
  @alive` looks the destination up in the main table: it is the uplink's
  packets that go untracked, as long as the main table routes them there.
  Source-based policy routing (an `ip -6 rule from <prefix> table <n>` that
  sends clients out of the uplink while the main table's default goes
  elsewhere, or the reverse) makes the rule untrack the wrong packets: a
  flow that leaves through a tunnel with its replies unmarked then has no
  conntrack entry and is rejected. nftables offers only `fib daddr . mark
  oif`, so such a setup needs a mark set per routing rule or an `oifname`
  match in a mangle chain instead. Whether the gateways route by source is
  to be checked against bbb-configs before deployment (see "Open points").
- Adding or removing a gateway requires re-rendering all gateways.

## Open points

Not done or not verified yet:

- The package builds with the snapshot SDK and its BPF toolchain for x86_64,
  mipsel_24kc, aarch64_generic and mips64_octeonplus (what the repository's
  CI builds), the snapshot's bpf-headers (6.12; the kernel is 6.18); the unit
  test built by the SDK's toolchain passes under qemu on mips64 big-endian
  and mipsel. OpenWrt 24.10 (kernel 6.6, bpf-headers 6.6) has not been
  built: the programs compile there (`BPF_FIB_LOOKUP_MARK` is left out, so
  the bypass does not see routing rules on the mark), the heartbeat uses
  destroy+add for it, nothing has run on it. No built package has been
  installed anywhere.
- The programs use BPF helpers only, no kfuncs: a kfunc call needs the
  kernel's own BTF, which no OpenWrt target builds, and the MIPS JIT does
  not support kfunc calls. Conntrack kfuncs, dynptr parsing and the
  netfilter program type are therefore out of reach, by design.
- The programs must run JIT-compiled. MIPS boots with
  `net.core.bpf_jit_enable=0`; base-files sets it to 1 at boot. The daemon
  logs a warning and shows `jited 0` in its status when they are
  interpreted. Nothing has been measured on the EdgeRouter 4 yet: first
  `bpf_stats` per program, then pktgen with conntrack, programs, bypass.
- The daemon and the programs have not run on kernel 6.12 or on the
  edgerouter-4. The per-packet cost of the programs is unmeasured.
- Whether the gateways route forwarded traffic by source (policy routing
  rules) has not been checked; if they do, the outbound notrack rule needs
  another predicate (see "Known limits").
- The tcx attachment needs kernel 6.6 and libbpf 1.3 (OpenWrt 24.10 has
  6.6 and 1.5). It has run next to an ingress qdisc with an IFB redirect
  and fq_codel in the VM (dptest `ingress_qdisc`), not next to sqm-scripts
  on a device.
- The mark a reassembled packet inherits is kernel behaviour read from the
  6.18 source and tested on 7.3.
- The notrack rules cover what leaves through the uplink and what comes in
  marked; forwarding between mesh interfaces is tracked as before, so
  bbb-configs' own `NOTRACK` rules keep their job. The per-packet cost of
  the set lookup on the outbound side is unmeasured.
- The bypass has forwarded into veth, GRE and WireGuard devices in the VM
  only; its gain on the gateways' hardware is unmeasured.
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

`test/gwlab` holds the data path tests, documented in
[test/gwlab/README.md](test/gwlab/README.md): a lab of clients, gateways and
servers with per-link latency in network namespaces, real traffic over every
pair of gateways, judged by what the endpoints see. It knows no
implementation; the adapter for this daemon is `test/gwlab/impl/flowsync.py`,
and `flowsync_conntrack`, `conntrackd` and `none` are there to compare
against. BPF programs cannot be loaded from a user namespace, so the lab
needs real root for this daemon: `make itest` runs it directly as root and
otherwise in a throwaway VM on the host's kernel (virtme-ng, `VNG=`).

    make -C src test                                  # unit test
    make -C src itest                                 # scenario steady, all fleets
    make -C src itest S=symasym FLEET=strict          # another scenario, one fleet
    make -C src check                                 # unit test, dptest, steady

`test/dptest` is the layer below, documented in
[test/dptest/README.md](test/dptest/README.md): what is specific to this
datapath and cannot be judged from the endpoints. Two or three gateways
without link latency; the scenarios look into the flow tables, the firewall's
counters and the daemon's status: fragments and extension headers, the
bypass, the place of the accept rule behind the MSS clamp, rules and programs
being put back, protocols without ports, the first start.

    make -C src dptest                                # all scenarios
    make -C src dptest DS="frag_asym bypass"          # some
