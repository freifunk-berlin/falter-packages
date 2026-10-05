# flowsync

Scoped conntrack flow announcer for the Freifunk Berlin active-active IPv6
gateways. Replaces conntrackd (Mode NOTRACK) + samplicator.

Outbound traffic of a mesh client may leave through gateway A while the reply
enters through gateway B. B runs a stateful fw4 firewall towards the mesh and
only accepts the reply if it has a conntrack entry for the flow. `flowsync` on A
announces the flow to all other gateways; they inject it into their conntrack
table, and fw4's existing `ct state established,related` rule accepts the reply.

## Design

- **Soft state.** A record means "this flow exists now". Injected entries carry
  their own timeout (`element_timeout`) and expire locally. No sequence numbers,
  acks or caches. Lost datagrams are repaired by the next periodic refresh; a
  gateway that lost state (restart, reboot, flush) asks its peers for a round
  at once (see "Liveness and resync").
- **No DESTROY propagation.** DESTROY events do not say why an entry died; on a
  gateway that never sees the flow it is a plain timeout. Nothing is sent on
  expiry.
- **Inject into conntrack**, no nftables set or extra rule.
- **Only what needs it.** IPv6, UDP and TCP, client inside a configured `prefix`.
  Protocols are configured per gateway (`list proto`); the default is UDP and
  TCP.
- **N-way fan-out:** one datagram per configured peer.
- **Nothing lives on announcements alone.** A peer can never refresh an entry a
  gateway did not create, a copy announces only with evidence of a packet since
  the last round, and ownership of copies is re-learned from the kernel every
  round. See "Loop prevention".
- **Filter in the kernel.** A BPF filter on the event socket and status, mark and
  protocol filters on the table dumps keep IPv4 and TCP-when-not-wanted entries
  from reaching user space; on the event socket also foreign-prefix entries.
- **One thread, no blocking.** Events, datagrams, injection replies and the
  refresh dump are all driven from one `poll()` loop; the dump is read one chunk
  at a time, paced by `tx_rate`.
- **Binary UDP transport, no keys.** See "Known limits".

### Loop prevention

Copies are injected with `SEEN_REPLY` already set (never `ASSURED`). The UDP
tracker (`nf_conntrack_proto_udp.c`) sets `ASSURED` on the first packet it sees
in *either* direction once `SEEN_REPLY` is set; the TCP tracker
(`nf_conntrack_proto_tcp.c`) on the first ACK or data segment the copy's
`ESTABLISHED` state accepts (with `be_liberal`, any; a SYN/ACK, FIN or RST
does not set it). So the kernel itself tells us whether a gateway sees the
flow:

| entry | status | announced |
|---|---|---|
| native, replies take another gateway | `!SEEN_REPLY` | yes |
| native, symmetric path | `SEEN_REPLY`, `ASSURED` | yes |
| copy, this gateway forwards or sees replies | `SEEN_REPLY`, `ASSURED` | only with a packet since the last round |
| copy, this gateway sees nothing | `SEEN_REPLY` only | **no** |

Three rules make sure that no set of entries can keep itself alive once the
real traffic has stopped:

1. **A peer never refreshes an entry this gateway did not create.** Every
   announcement for a tuple that is not a copy of our own is injected with
   `NLM_F_EXCL`; if an entry exists the kernel answers EEXIST and leaves it
   alone. A native entry therefore lives on packets only, and its existence is
   its liveness.
2. **A copy announces only with evidence of traffic.** A refresh sets the
   timeout to exactly `element_timeout`; a packet sets it to the protocol's
   natural value (OpenWrt: UDP 60 s, UDP stream 180 s, TCP unacknowledged
   300 s, established 7440 s). A copy is announced when it is `ASSURED` and its
   remaining timeout deviates from what plain decay since the last known value
   would leave, in either direction, which only a packet can cause (a natural
   timeout below `element_timeout`, such as an unacknowledged segment cutting
   an established copy, counts as well). The last known value is
   the later of what the previous dump saw and what our own create or refresh
   set. A copy without a known value (after a restart or a lost table slot)
   is not announced in that round; it can show evidence from the next one.
3. **Ownership comes from the kernel.** Every round the table dump includes our
   marked entries; the daemon learns from it which copies it owns (so refreshes
   go through) and forgets the ones that are gone. A daemon restart, a slot
   eviction or a `conntrack -F` heal within one round. Any DESTROY event of a
   copy, expired or not, drops the ownership at once, so the next
   announcement creates the copy again; a dump chunk that the kernel
   generated before that DESTROY (one waits in the dump socket while the
   daemon parses the previous one) does not make it ours again. A packet that
   creates a native entry for a tuple we believed to hold a copy of drops the
   ownership too (the NEW event), so a peer's next announcement is an EXCL
   create that leaves the native alone (`rx_own_lost`).

Every refresh thus traces back to a packet within the last round on some
gateway. When the traffic stops, natives expire on their natural timeouts,
announcements stop within a round, and all copies without traffic of their
own expire within `element_timeout` plus one `interval`. Without these rules two gateways that each forwarded one
direction of a flow would refresh each other's entries forever.

A side effect worth knowing: copies without traffic stay non-`ASSURED`, so a
full table may evict them (`early_drop` takes a non-`ASSURED` entry from the
buckets near the new entry's, for creates through ctnetlink only from the first
few buckets), like any other non-`ASSURED` entry, not before them.

### Filter policy

Applied identically on TX and RX, on the original tuple (client -> server):

- L3 is IPv6 and L4 protocol is in `proto` (default `udp`, `tcp`)
- client is inside at least one `prefix` and inside no `exclude`
- server is inside no `exclude_dst` (the mesh prefix, no default)
- for UDP, the server port is not in `skip_server_port` (default `53`)
- both ports are 1..65535

Both addresses must be routable (not unspecified, loopback, multicast,
link-local or v4-mapped), and on RX the server must not be one of the
receiving gateway's own addresses (a flow to the gateway itself is never
asymmetric, and a copy would let it in as established on any interface; put
networks behind the gateways into `exclude_dst`).

A spoofed sender can therefore at most create entries for (client inside our
prefixes) x (server outside the mesh), each living `element_timeout` seconds.

### TX

- Subscribes to conntrack NEW events. The subscription carries a kernel-side BPF
  filter (built with libnetfilter_conntrack) that drops everything but the
  configured protocols, IPv6, and clients inside the configured prefixes (up to
  20 prefixes; with more, prefixes are checked in user space only), and drops
  the events of our own copies (our mark): every copy we inject comes back as a
  NEW event, and while a resync answer re-creates thousands of them those
  echoes would overrun the socket and take real flows' events with them. `exclude`,
  `exclude_dst` and ports are always checked in user space. Every wanted native
  entry that is announceable (see above) is sent immediately to all peers.
- Every `interval` seconds the IPv6 table is dumped per configured protocol in
  four kernel-filtered phases: native entries with `status & SEEN_REPLY == 0`
  (for TCP: without `ASSURED`, see below), native entries with `ASSURED`,
  offloaded native entries that are
  `SEEN_REPLY` but not `ASSURED` (see "Flow offloading"; UDP only, and only
  while the `nf_flow_table` module is loaded or an offloaded entry was seen:
  every phase walks the whole table with softirqs off), and our marked
  copies whatever their status
  (`CTA_MARK`/`CTA_MARK_MASK` and `CTA_FILTER` on the protocol). Natives are
  announced; copies are announced only with traffic evidence (rule 2) and
  re-learned as ours (rule 3). The dump is streamed: one 32 KiB chunk is read
  per loop iteration and only while the send queue has room for it. Queued
  entries are announced at `tx_rate` datagrams per second per peer. A round
  whose entries are not all sent when the next `interval` comes is not
  restarted and nothing is dropped; the next round is delayed instead and
  starts as soon as the previous one is sent, the interval counting from
  there (`refresh_overrun`, raise `tx_rate`). Only a dump that delivers no chunk for
  `2 x interval` while it has room in the queue is considered stuck and
  started over (`refresh_errors`); a round held back by its own send queue is
  slow, not stuck.
- A TCP native that is `SEEN_REPLY` but still `SYN_SENT` is promoted: its
  client's SYN passed this gateway, the SYN/ACK took another one, and then the
  reply path moved here. The tracker ignores the server's segments in
  `SYN_SENT` (accepted as established, but not refreshed), so the busy
  connection would die at the SYN_SENT timeout (120 s after the SYN) and its
  next server segment would be reset. The entry is set to `ESTABLISHED` with
  `be_liberal`, in place (no `NLM_F_CREATE`, mark and timeout untouched), the
  next packet makes it `ASSURED` and the next round announces it as a
  symmetric native (`tx_promoted`). It is the gateway changing its own native,
  not a peer's announcement; the rest of the class, handshakes in progress, is
  left alone.
- UPDATE events are ignored; DESTROY events of our own copies are read for
  ownership and resync (see "Liveness and resync"). On ENOBUFS the kernel drops
  events until the socket queue has been read empty, without reporting the
  overrun again; what is queued is intact and is processed (`ev_overruns`).
  Once the queue is empty, the next round is pulled forward to one second from
  then instead of waiting up to `interval`, at most once per interval; a round
  that starts during the overrun does not count as the repair, because the
  kernel may still drop the events of flows created after its dump walked past
  them. Other socket errors reopen the subscription. An announcement that a
  full send buffer could not take for some peer pulls the same round.

### RX

- UDP socket on `bind_address`:`port`, bound to the `interface` device. The
  kernel binds a socket to the device's index, so the daemon looks every
  `interval` whether the device still has that index and reopens the socket
  if not (netifd deletes and re-creates a VLAN, bridge or bond device on every
  `ifup` of its interface; the old socket would receive nothing and fail every
  send), then asks its peers for a resync. A device that does not exist yet
  at startup is waited for the same way, and the bind address may be missing
  too (`IPV6_FREEBIND`). Datagrams from addresses not listed as `peer` are
  dropped (logged with their source, rate limited), and so are IPv6
  datagrams whose source is a v4-mapped address (the IPv6 stack lets them
  through, and to a dual-stack socket they look exactly like IPv4 datagrams
  from that address; only real IPv4 datagrams carry `IP_PKTINFO`).
- Records are decoded and checked against the policy. A per-tuple table
  (131072 slots, linear probing) remembers when each tuple was last announced
  and injected and whether its entry is a copy of ours. An announcement of a
  tuple that is our copy is only noted; it is refreshed by our own dump (see
  "refresh" below). A create for the same tuple from any peer within
  `interval`/2 is applied once.
- The socket is drained (up to 64 datagrams per loop iteration) and the burst is
  injected in one netlink batch of `IPCTNL_MSG_CT_NEW` messages:
  - **create** (tuple not known as our copy): `NLM_F_CREATE|NLM_F_EXCL`,
    original tuple client -> server, reply tuple derived, `CTA_TIMEOUT` =
    `element_timeout`, `CTA_STATUS` = `SEEN_REPLY|CONFIRMED`, `CTA_MARK` =
    `ct_mark` / `CTA_MARK_MASK` = `ct_mark_mask`, and for TCP
    `CTA_PROTOINFO_TCP` (see "TCP"). `CONFIRMED` must be echoed: the kernel sets
    it on the new entry before it looks at `CTA_STATUS` and refuses any status
    that differs in that bit with EBUSY. An existing entry answers EEXIST and is
    left untouched (`inject_exists`).
  - **refresh** (our copy, decided by our own dump, see below): neither
    `NLM_F_EXCL` nor `NLM_F_CREATE`, both tuples and `CTA_TIMEOUT` only. The kernel applies the timeout and nothing
    else, so a refresh can never change a copy's status, mark or TCP state.
    ENOENT means the copy is gone (expired, flushed, evicted); it is counted
    (`inject_gone`) and re-created at once. (With `NLM_F_CREATE` the
    kernel would create an unmarked, unreplied entry without TCP state from the
    refresh instead; it would look like a native and be announced as one.)
- Every injected message is remembered by its netlink sequence number so the
  kernel's error report, which arrives synchronously with the batch, can be
  attributed to the record and to its table slot.
- Other injection errors are counted and logged (rate limited), never retried.
- Refreshes are decided by our own dump, which sees every copy of ours once
  per round with its current remaining timeout. A copy is refreshed when a
  peer has announced it since the dump last looked (one lost datagram costs
  one refresh, `element_timeout` covers it) and its remaining timeout is at
  most `element_timeout` plus one `interval`. Without announcements nothing
  refreshes a copy and it expires; nothing keeps itself alive.
- Once the gateway sees a packet of the flow, the entry is `ASSURED` and kept
  alive from traffic (UDP stream timeout, TCP timeouts) independent of
  announcements. A refresh sets the timeout to exactly `element_timeout`, it
  does not extend it, so a copy with more time left is held, not refreshed
  (`inject_held`): it lives on its own traffic like a native entry would, and
  an idle TCP connection keeps its state on the reply gateway for the
  established timeout instead of losing it when the forward gateway falls
  silent. Deciding this when an announcement arrives would not work: a packet
  may have raised the timeout since the last dump, and the refresh would cut
  it.
- A TCP create that hits EEXIST is looked up (once per tuple and
  `element_timeout`). If the existing entry is the flow in reverse
  (server -> client), unreplied and unmarked, it is a reply packet that came
  before the copy and was let through by a stateless rule; the kernel picked
  it up as a connection of its own, and it would block the copy for as long
  as the server keeps sending. It is deleted by its id (the kernel refuses if
  the entry was replaced by another one meanwhile, though not if the same
  entry changed) and the copy created in the same batch
  (`inject_replaced`). A server segment between the delete and the create (on
  another CPU) makes a new pickup and the create fails with EEXIST; that is
  looked up again at the next datagram or dump chunk, not an
  `element_timeout` later (at tens of thousands of segments per second the
  race is lost often). Every other existing entry stays untouched. UDP creates
  that hit EEXIST are not looked up: there is nothing to replace.
- There is no close propagation. When a flow ends, the announcements stop and
  the peer copies expire within `element_timeout` plus one `interval` (the
  last refresh can come from the dump after the last announcement); copies
  that carried traffic expire on their own timeout.

### Wire format

Binary UDP. One datagram is a 4 byte header followed by `count` records of 40
bytes, at most 34 records (1364 bytes) per datagram. The sender's default is 30
records (`batch_lines`), 1252 bytes as an IPv6/UDP packet, so that a path
which drops packets above the IPv6 minimum MTU of 1280 bytes loses no
refresh; receivers accept up to 34:

    header:  version(1)=1  count(1)  flags(1)  reserved(1)=0
    record:  proto(1)  flags(1)=0  cport(2)  sport(2)  reserved(2)=0
             client(16)  server(16)

Ports in network byte order, addresses as in the packet. `proto` is the IP
protocol number (17 for UDP, 6 for TCP). A datagram whose version byte is not 1 is counted
as `rx_version` and dropped; one whose length does not match `count` is counted
as `rx_parse`. The old text format started with the byte `'1'` (0x31), which
shows up as `rx_version`. A datagram with `count` 0 is a heartbeat; header
flag `0x01` asks the receiver for a round now (resync). Record `flags` and the
reserved bytes are zero.

### Liveness and resync

- Every `interval` each daemon sends a heartbeat (a datagram without records)
  to all peers (`tx_control`, `rx_control`). The status file shows per peer
  when the last datagram came (`peer <address> rx <n> age <s>`), and a peer
  silent for three intervals is logged (and logged again when it is back).
- A gateway that lost copies asks its peers for a round instead of waiting up
  to one `interval` for their next one (`tx_resync`): at startup (a restart or
  reboot, the table may be gone and refreshes were missed while the daemon
  was down) and when one of its copies is destroyed early. A DESTROY event
  carries the remaining timeout only if the entry had time left, so a copy
  destroyed with time left was flushed (`conntrack -F`), deleted or evicted,
  never expired (`copies_lost`). The daemon subscribes to DESTROY events,
  kernel-filtered on its mark; every one of them, expired or not, ends the
  ownership of that copy, only those with time left ask for a round. The request goes out only once every queued
  DESTROY event is read: an answer for a copy whose destruction is still
  queued would be taken for a refresh of a copy we own and not re-create it.
  If that socket overruns (a flush of many
  copies), which copies are gone is unknown: the daemon assumes none of its
  copies exists any more, every announcement becomes a create with EXCL, a
  copy that does exist answers EEXIST and the next dump learns it again.
- A peer serves a request (`rx_resync`) by starting its next round at once,
  at most once per requesting peer per `interval`/4, and it pulls rounds
  forward for requests at most twice per `interval` whoever asked (two in a
  row, then the budget refills; a round started meanwhile serves every
  request, and forged requests cannot keep a gateway in back-to-back rounds).
  A requester asks at most once per `interval`/2; a lost request (a single
  datagram) costs the wait for the peers' next regular round, and a request
  that could not be sent at all (no socket while the interface is away, every
  send failed) stays pending. If the peer is busy
  with an overrunning round when the request comes, it is served by the round
  after that one, not dropped. A round pulled forward for a request is sent at
  `resync_rate` (default four times `tx_rate`), still bounded by the send
  buffer: until it is through, the requester has no copy of those flows, and
  their replies meet the firewall. The window is about the peer's announced
  entries / (`resync_rate` x `batch_lines`): with the defaults 60000 records/s,
  under two seconds for 100k entries.
- The DESTROY socket can overrun too (`ds_overruns`, a flush of many copies):
  then which copies are gone is unknown, and ownership of all of them is
  dropped, once when the overrun is reported and again when the queue has
  been read empty (the kernel drops events until then).

## Configuration

The daemon is configured on the command line only; it does not read any file.
The init script renders `/etc/config/flowsync` (written by bbb-configs) into
these options.

    flowsync [options] <command>
      run                                        run the daemon
      check                                      print the parsed configuration
      status                                     print counters, peers, conntrack count
      announce <client> <cport> <server> <sport> [proto] send one record to all peers

| option | UCI option | default | meaning |
|---|---|---|---|
| `-b, --bind ADDR` | `bind_address` | any | local address for receiving and sending; peers check the source address, so set it to the address the peers list. IPv4 or IPv6. |
| `-I, --interface DEV` | `interface` | any | the uplink device: sync datagrams are accepted only when they arrive on it (`SO_BINDTODEVICE`). Peers are recognised by source address alone and the kernel accepts a datagram for any local address on any interface, so without it a host on the mesh side can send with a peer's address; the daemon warns at startup if it is unset. The UCI option may name the logical interface (`uplink`), the init script passes its device. A device that does not exist yet is waited for, a re-created one is followed (see "RX") |
| `-p, --port N` | `port` | `3780` | UDP port, the same on all gateways |
| `-i, --interval SEC` | `interval` | `30` | seconds between refresh rounds and counter logs |
| `-t, --element-timeout SEC` | `element_timeout` | `90` | timeout of injected entries; must be at least `3 x interval` (the default) and should stay below the protocols' natural timeouts (UDP stream 120 s), see "Known limits" |
| `-l, --batch-lines N` | `batch_lines` | `30` | records per datagram, 1..34; above 30 the path between the gateways must carry 1412 byte IPv6 packets |
| `-r, --tx-rate N` | `tx_rate` | `500` | refresh datagrams per second, per peer |
| `-R, --resync-rate N` | `resync_rate` | `0` | the same for a round that answers a peer's resync request; `0`: four times `tx_rate` |
| `-B, --rcvbuf BYTES` | `rcvbuf` | `8388608` | receive buffer of the UDP and the conntrack event socket; set with `SO_RCVBUFFORCE` (the daemon has `CAP_NET_ADMIN`), falling back to `SO_RCVBUF`, which `net.core.rmem_max` caps |
| `-m, --ct-mark HEX` | `ct_mark` | `0x01000000` | mark set on created entries; must be non-zero and inside the mask. Hexadecimal with or without `0x`; all other numbers are decimal |
| `-M, --ct-mark-mask HEX` | `ct_mark_mask` | `0x01000000` | mask of that mark |
| `-C, --max-copies N` | `max_copies` | `0` | most copies this gateway holds; `0`: a quarter of `nf_conntrack_max` (unlimited if that is 0), at most 98304 (three quarters of the per-tuple table). Beyond it no new copy is created (`rx_limited`). A gateway holds copies of the synced flows of all other gateways, so this bounds the fleet's flows, not this gateway's |
| `-c, --max-copies-per-client N` | `max_copies_per_client` | `0` | most copies of the flows of one client prefix (`client_prefix_len`); `0`: no limit. Beyond it no new copy is created for that prefix (`rx_limited_client`), so one client with thousands of flows (a scanner, P2P) cannot use up the pool for everybody. The limit refuses in arrival order, not by need; in a Freifunk network one /64 is a whole location's client network |
| `-L, --client-prefix-len N` | `client_prefix_len` | `64` | the prefix length a client is counted by for `max_copies_per_client`, 1..64. A host can source from every /64 of its location's delegated prefix (the gateways do not filter sources), so a per-/64 limit stops a single client, not a location: count by the delegation (`56`) for that |
| `-P, --proto NAME` | `proto` (list) | `udp`, `tcp` | repeatable; `udp` and `tcp` |
| `-S, --skip-server-port N` | `skip_server_port` (list) | `53` | repeatable; UDP server ports never synced (TCP to these ports is synced) |
| `-e, --peer ADDR` | `peer` (list) | - | repeatable; the other gateways (max. 32) |
| `-x, --prefix CIDR` | `prefix` (list) | - | repeatable; synced client prefixes |
| `-X, --exclude CIDR` | `exclude` (list) | - | repeatable; client prefixes not synced |
| `-D, --exclude-dst CIDR` | `exclude_dst` (list) | - | repeatable; server prefixes not synced |
| `-s, --status-file PATH` | - | `/var/run/flowsync/status` | where `run` writes its counters |
| `-u, --user NAME` | - | - | after opening its sockets, run as this user with `CAP_NET_ADMIN` only (the init script passes `flowsync`, the package creates the user); without it the daemon stays root but drops every other capability |
| `-d, --debug` | `debug` | off | log every record sent, received and injected |

Giving a repeatable option replaces its default (`--proto tcp` alone would drop
`udp`). There is no default for `exclude_dst`; the mesh prefix must be configured
(`2001:bf7::/32` for Freifunk Berlin), otherwise mesh-internal flows are synced too.
`element_timeout < 3 x interval` is refused: one lost datagram would expire
entries before the next refresh. At startup the daemon warns if
`nf_conntrack_udp_timeout_stream` or `nf_conntrack_tcp_timeout_unacknowledged`
is not above `element_timeout + interval` (a copy that carries traffic would
then be refreshed down to `element_timeout` every round instead of living on
its own timeout), and
logs an error if `nf_conntrack_events` is 0 (the kernel then sends no
conntrack events at all: new flows wait for the next round, lost copies go
unnoticed).

Logging goes to syslog (tag `flowsync`) when the daemon is started by procd,
and to the terminal with timestamps (`HH:MM:SS [level] message`) when
`flowsync run` is started from a shell. `--debug` adds one line per record and
per injection batch, which is a lot under load, and `syslog()` can block on a
slow logd, so do not leave it on.

### UCI (OpenWrt)

`/etc/config/flowsync`, rendered by bbb-configs:

    config flowsync 'main'
        option enabled '1'
        option debug '0'
        option bind_address '<this gateway uplink IPv4>'
        option interface '<the uplink device>'
        option port '3780'
        option interval '30'
        option element_timeout '90'
        option batch_lines '30'
        option tx_rate '500'
        option rcvbuf '8388608'
        option ct_mark '0x01000000'
        option ct_mark_mask '0x01000000'
        option max_copies '0'
        option max_copies_per_client '0'
        option client_prefix_len '64'
        option resync_rate '0'
        list proto 'udp'
        list proto 'tcp'
        list skip_server_port '53'
        list peer '<other gateway uplink IPv4>'    # one per peer
        list prefix '2001:bf7:750::/44'            # one per prefix
        list exclude_dst '2001:bf7::/32'
        list exclude '<prefix filtered elsewhere>' # zero or more

`enabled` is evaluated by the init script; unknown options are ignored. A config
change restarts the daemon (procd reload trigger + file watch).
`/etc/init.d/flowsync check` prints the configuration as the daemon parses it.
The init script starts the daemon at `START=21`, right after the network and
long before bird (70) attracts traffic, so that the copies are in place when
replies arrive; the daemon waits for its interface by itself. A daemon that
cannot start (bad configuration) is respawned at most 5 times an hour.

### Privileges

The daemon starts as root (procd), opens its sockets and binds them, then
drops every capability but `CAP_NET_ADMIN` (ctnetlink needs it for every
message) and, with `--user`, switches to that user; `no_new_privs` is set.
The UDP parser, the part exposed to the network, thus never runs with full
root. The status file lives in `/var/run/flowsync/`, which the daemon creates
for that user before it switches (and hands over if root owns it).

### Development builds

The package targets OpenWrt. For development it builds on any Linux with
libnetfilter_conntrack and libmnl (Debian: `libnetfilter-conntrack-dev
libmnl-dev`), see "Source layout and tests".

## Operation

On a gateway: `/etc/init.d/flowsync status|check|announce ...` (the init script
renders the UCI options). `flowsync status` needs no configuration, it reads the
status file.

Every `interval` the daemon logs its counters to syslog (tag `flowsync`) and
writes them to `/var/run/flowsync/status`:

| counter | meaning |
|---|---|
| `tx_events` | records announced from NEW events |
| `tx_scanned` | entries returned by the refresh dumps (natives of other protocols and natives that are `SEEN_REPLY` but not `ASSURED` are filtered in the kernel and not counted) |
| `tx_refresh` | records announced from the refresh dumps, natives and copies |
| `tx_copies` | of these, copies announced because they saw a packet since the previous round |
| `tx_promoted` | TCP natives stuck in `SYN_SENT` although replies pass them, set to `ESTABLISHED` (see "TX") |
| `tx_datagrams` / `tx_errors` | datagrams with records sent / send errors (per peer, also in the peer lines). Sends never block: a refresh datagram that a full send buffer cannot take waits and the refresh with it (a slower round); an announcement of a new flow is lost then, counted, and a round is pulled forward to repair it |
| `tx_control` / `tx_resync` | heartbeats and resync requests sent (per peer) / resync requests made |
| `tx_refresh_dropped` | refresh entries dropped because the queue was full (should stay 0) |
| `refresh_rounds` | completed refresh rounds |
| `refresh_overrun` | interval ticks at which the previous round was still running (raise `tx_rate`) |
| `refresh_errors` | dump socket or request errors; the round is aborted |
| `ev_recv` | messages read from the event socket (after the kernel filter) |
| `ev_own` | NEW events of our own injections that got past the kernel filter, ignored (0 while the filter is attached) |
| `ev_overruns` | event socket overruns (ENOBUFS); events were lost, an early round after the overrun repairs |
| `ds_overruns` | DESTROY event socket overruns (or reopens); ownership of all copies dropped, a resync request follows |
| `rx_datagrams` | datagrams with records received |
| `rx_control` / `rx_resync` | heartbeats and resync requests received / resync requests received |
| `rx_records` | records accepted for injection |
| `rx_bad_peer` | datagrams from a non-peer source (the source is logged, rate limited: a peer whose source address is not the configured one shows up here) |
| `rx_policy` | records rejected by the policy |
| `rx_parse` / `rx_version` | malformed datagrams or records / unknown format version |
| `rx_dup` | records deduplicated |
| `rx_evictions` | per-tuple table slots taken from a live tuple (table too small for the flow count) |
| `rx_limited` | announcements of new flows refused because `max_copies` was reached (forged announcements, or a limit too low for the fleet) |
| `rx_limited_client` | announcements of new flows refused because the client's prefix holds `max_copies_per_client` copies (a client with very many flows) |
| `rx_own_lost` | copies replaced by a native entry (a packet created one after the copy was gone) |
| `inject_created` | conntrack entries created |
| `inject_refreshed` | timeouts of our own copies refreshed |
| `inject_held` | refreshes skipped because the copy lives on its own traffic (a refresh would shorten it) |
| `inject_exists` | creates refused because an entry exists that is not ours (a native, or a copy from before a restart); left untouched |
| `inject_replaced` | reversed, unreplied entries (a reply that came before the copy) replaced by the copy |
| `inject_gone` | refreshes of a copy that no longer existed; re-created at once |
| `copies_lost` | our copies destroyed with time left (flush, delete, eviction); each triggers a resync request |
| `inject_errors` | other conntrack failures |

and these gauges:

| gauge | meaning |
|---|---|
| `refresh_running` | a refresh round is in progress |
| `refresh_entries` | entries queued by the last complete round |
| `refresh_ms` | wall time of the last complete round |
| `loop_max_ms` | longest time the loop spent in handlers between two polls during the last interval; tens of milliseconds are fine, seconds mean a stall |
| `copies` | our copies in the table at the last complete round |
| `copies_live` | of these, with a packet since the previous round |
| `copies_offloaded` | of these, in the flowtable (fw4 flow offloading); counted as live |
| `owned` | our copies: per-tuple table slots that own one, kept up to date as copies are created and destroyed (what `max_copies` limits; `max_copies` and `max_copies_per_client` follow in the status file) |

and one line per peer, `peer <address> rx <datagrams> age <seconds since the
last one> tx_errors <n>` (`age never` if nothing came yet; `tx_errors` counts
datagrams to that peer lost to send errors). Every peer sends a heartbeat
each `interval`, so an `age` of more than a few intervals means the peer or
the path to it is down.

`flowsync status` (also part of `/etc/init.d/flowsync status`) replaces the old
`conntrackd -s` health check. The status file is written once per `interval`,
so counters lag by up to that.

## Verification on a device

1. `/etc/init.d/flowsync status; logread -e flowsync` shows counters, no errors,
   and a `kernel filter` line with the number of prefixes installed.
2. On gw A:
   `/etc/init.d/flowsync announce 2001:bf7:750:3f00::1 50000 2a00:1450:4001:81a::200e 443`.
   On gw B: `conntrack -L -f ipv6 -p udp --mark 0x01000000/0x01000000` shows the
   entry with about 90 s left, neither `[UNREPLIED]` nor `[ASSURED]`; B's
   `inject_created` went up by one. Re-run `announce` after 60 s: the entry is
   refreshed by B's next dump round after that announcement (not when it
   arrives), back to 90 s, and `inject_refreshed` went up by one. Without a
   new announcement B does not refresh it and it expires. (Confirm the `--mark
   value/mask` syntax of conntrack 1.4.8 on the device; the `conntrack` tool is
   not in the gateway image by default.)
   For TCP (with `list proto 'tcp'` on both gateways): `announce ... tcp` and
   `conntrack -L -f ipv6 -p tcp --mark 0x01000000/0x01000000` on B shows the
   entry `ESTABLISHED` with about 90 s left and no `[ASSURED]` until B forwards
   a packet of it.
3. Loop check: B's `tx_events`/`tx_refresh` must not increase for the injected
   entry (the kernel filter keeps its NEW event from B's daemon; `ev_own`
   counts one only if that filter could not be attached), and A's
   `rx_records` must not increase.
4. Real flow from a client in a synced prefix: `curl --http3 -6
   https://www.google.com`, a WireGuard handshake to an external endpoint,
   `ntpdate -q` to an IPv6 NTP server. On the gateways not on the outbound path,
   `conntrack -L -f ipv6 -p udp -d <client>` shows the entry turn `[ASSURED]`
   when replies take that path; that gateway then announces the flow as well
   (`tx_copies` increases, A logs `inject_exists`, and A's own entry stays
   unmarked). Flows to port 53 or to 2001:bf7::/32 are never injected.
   Unsolicited inbound UDP from outside stays rejected.
   For TCP, a long-lived asymmetric IPv6 TCP connection from a client in a
   synced prefix is accepted on the reply gateway, whose entry turns `[ASSURED]`
   once it forwards a packet.
5. Stale flows: after a client's UDP flow has ended, every gateway's entry for
   it must be gone within about `element_timeout` plus the kernel timeout of the
   last native entry (a UDP stream dies 120 s after its last packet). Watch
   `copies` and `nf_conntrack_count` over a quiet period; both must come down.
6. Load on the edgerouter-4 during a refresh: `top -d 5`, `netstat -su`
   (`RcvbufErrors`), `conntrack -C`, and `refresh_ms` / `loop_max_ms` in the
   status. `ev_recv` should track IPv6 NEW events of the configured protocols
   only; compare with `conntrack -E -e NEW -f ipv6 | pv -l` if in doubt.
7. Kill the daemon (procd respawns it); the copies on this gateway survive, the
   restarted daemon re-learns them within one round (`owned` comes back,
   `inject_exists` stays small, the peers' `rx_resync` counts the restart).
   Do not test recovery with `conntrack -F` on a gateway that carries traffic:
   it deletes the gateway's own entries too, and with the gateways' forward
   policy REJECT every server segment of a connection without an entry is
   answered with a TCP reset once the stateless budget is used up (see
   "Known limits"). On a gateway drained of traffic (BGP down), `conntrack -F`:
   `copies_lost` counts, the peers serve the resync request and the entries are
   back within about (the peers' announced entries) / (`resync_rate` x
   `batch_lines`) seconds.

## Migration from conntrackd

flowsync and conntrackd (with samplicator) both use UDP port 3780 and cannot
read each other's datagrams (conntrackd counts flowsync's as bad size,
flowsync counts conntrackd's as `rx_version`). While the fleet is mixed, every
asymmetric flow whose forward and reply gateway run different daemons has no
entry on the reply gateway: UDP replies are rejected, TCP lives on the
stateless budget. With k of n gateways migrated that is 2k(n-k)/(n(n-1)) of
those flows, 33 % for one of six, 60 % for three of six. So never run a
canary or a rollout over days:

- **One window:** build all images in one change and flash the gateways back
  to back (`sysupgrade` reboots, so there is no conntrack state to clean up),
  or in two halves within minutes.
- **Or a scripted flip:** ship an image with both packages, conntrackd active
  and flowsync configured with `enabled '0'`; then stop and disable conntrackd
  and samplicator and start flowsync on all gateways within seconds. Rollback
  is the reverse. A later image drops conntrackd.

Render all peers from the start; a peer still running conntrackd shows as
`age never` in `flowsync status`. Keep the stateless ACK/RST accept and the
sync firewall rule.

## Known limits

- The race between a fast reply and the announcement is RTT dominated and
  inherent to any sync design. QUIC and WireGuard retransmit, TCP retransmits
  the SYN/ACK.
- Sync is unauthenticated. The flow headers already cross transit networks
  unencrypted, and the impact of a spoofed record is bounded by the receiver's
  policy (a 90 s entry for one external address towards one UDP or TCP port of
  one client in a synced prefix), the same exposure conntrackd had. An outbound
  TCP SYN from a synced prefix creates a 90 s ESTABLISHED copy on every peer,
  just like any UDP packet does, within the same policy bound. Anybody who can
  send from a peer's uplink address (UDP needs no handshake) can make a
  gateway create copies. Set `interface` to the uplink: otherwise that is any
  host on the mesh side too (the kernel delivers a datagram for any local
  address on any interface, rp_filter is off by default, and Falter's freifunk
  zone accepts input). Without a bound about 2 Mbit/s of forged records
  would fill `nf_conntrack_max` (524288) and drop new flows of the gateway's
  own clients. `max_copies` bounds this: beyond it new copies are refused
  (`rx_limited`) while refreshes of existing copies go on, so a forger can
  degrade the sync but not the gateway. The GRE mesh between the gateways
  shares the trust model (GRE is accepted from any source).
- **`interface` does not keep the mesh out where the gateways masquerade.**
  A gateway that masquerades mesh traffic to its uplink (bbb-configs does,
  for IPv4) rewrites the source of a mesh host's datagram to its uplink
  address, which is exactly the peer address the other gateways accept, and
  forwards it out of the uplink: it arrives on the receivers' sync device from
  a valid peer. Any IPv4 client on the mesh can thus send announcements and
  resync requests to every gateway but its own exit, without spoofing. The
  daemon cannot tell such a datagram from its peer's. Only the firewall can:
  drop forwarded traffic towards the peers' sync port (or exclude the peers'
  addresses from masquerading) on every gateway.
- **The copy limits do not tell need from abuse.** `max_copies` and
  `max_copies_per_client` refuse new copies in arrival order. A host that opens
  unanswered flows by the hundred per second (each lives the SYN_SENT or UDP
  timeout plus `element_timeout` as a copy) can keep the pool full, and then
  legitimate flows get no copy on the gateways that hold it; set
  `max_copies_per_client` with `client_prefix_len` at the delegation size
  (`56`) to confine that to the host's own location.
- Adding or removing a gateway requires re-rendering all gateways (peer lists).
- ct mark bit `0x01000000` is free on the gateways today (qosify is not installed
  there). It is configurable; it marks the entries created by flowsync and is
  what tells copies from native entries, so nothing else may set or clear it on
  synced flows.
- Loop prevention relies on the UDP and TCP trackers setting `ASSURED`
  regardless of direction once `SEEN_REPLY` is set (kernel 6.12; for UDP only
  with 64-bit jiffies, so 64-bit kernels only: on 32-bit the stream check
  against a zero timestamp fails for half of each jiffies wrap), and on a
  packet changing the remaining timeout visibly. A natural timeout that equals
  what a refresh would leave at that moment (`element_timeout` minus the time
  since, e.g. TCP CLOSE_WAIT 60 s under continuous traffic with the defaults)
  is invisible while the copy is refreshed every round. Natural timeouts above
  `element_timeout + interval` keep a copy with traffic held rather than cut;
  with the OpenWrt values (UDP stream 180 s, TCP unacknowledged 300 s) and the
  defaults that holds.
- A flow is announced by every gateway that sees its packets, so with an
  asymmetric path two gateways announce it. Dedup on the receiver absorbs this;
  refresh traffic is at most twice that of a single announcer.
- A copy is announced from a packet only at the next round, so after a reroute
  the new carrier is heard up to one `interval` later (two after a restart of
  the carrier's daemon); third-party copies have
  `element_timeout` to spare. A copy that saw a packet is announced in the round
  after that packet and then falls silent; its own entry lives on until the
  protocol's natural timeout runs out.
- TCP entries live to the kernel's TCP timeouts when no close is seen, which on
  asymmetric paths is the normal case. That is conntrack's behaviour on every
  gateway already; OpenWrt sets `nf_conntrack_tcp_timeout_established` to
  7440 s (upstream default 5 days), `nf_conntrack_udp_timeout` to 60 s and
  `nf_conntrack_udp_timeout_stream` to 180 s.
- A reply that is accepted on a gateway *without* a copy, by any stateless
  rule (today's TCP accept for asymmetric replies, or an open port), creates a
  native entry in the reverse direction there (with `tcp_loose`, server data
  is picked up as `ESTABLISHED` with the server as the original side). For
  TCP the next announcement replaces it with the copy (see "RX"); for UDP the
  kernel refuses the copy with EEXIST as long as that entry lives
  (`nf_conntrack_hash_check_insert` checks both tuples). Such a pickup never
  gets `SEEN_REPLY`, so every one of its packets is `ct state new` and needs
  the stateless rule until the copy has replaced it.
- **The stateless ACK/RST accept is a hard budget, and the gateways reject.**
  Every window in which a gateway has no copy for a flow whose replies it
  carries (the first reply racing the announcement, a reboot or flush until the
  resync is through, a sync outage) puts that flow's server segments on the
  rate-limited stateless rule (5000 segments/s with a burst of 2500, for all
  flows of the gateway together, about 60 Mbit/s). Beyond it they fall through
  to the forward policy, which on the gateways is REJECT: the server gets a TCP
  reset whose sequence number it accepts, and the download dies. A window of
  T seconds with R uncovered segments/s rejects about
  (R - 5000)^2 x T / (2R) - 2500 segments. Measured after a flush at 20000
  server segments/s: 12000 copies were back in 0.9 s with about 1800 resets,
  40000 copies in 3 s with about 16000 resets (before `resync_rate`). Making the
  over-budget rules drop instead of falling through to REJECT turns that into
  retransmissions; that is a firewall change (bbb-configs).
- UDP replies in such a window get an ICMPv6 port unreachable (the only
  stateless UDP rule is for DNS, which flowsync does not sync). QUIC and
  WireGuard mostly ignore it, connected UDP sockets see ECONNREFUSED.
- An idle asymmetric TCP connection keeps its state on the reply gateway for
  `nf_conntrack_tcp_timeout_unacknowledged` (300 s) after the server's last
  data, not for the established timeout: the copy never sees the client's
  ACKs, so after two server segments the tracker caps it at the
  unacknowledged timeout. A server that pushes after a longer silence (IMAP
  IDLE, SSH, push notifications) relies on the stateless rule; raising
  `nf_conntrack_tcp_timeout_unacknowledged` on the gateways extends it, for
  natives too.
- The `element_timeout >= 3 x interval` check is local. Every gateway's
  `element_timeout` must cover every *peer's* `interval`; render the same values
  everywhere. A copy that received a RST is in state
  CLOSE but is kept at `element_timeout` by refreshes until its origin stops
  announcing it.
- If a refresh round takes longer than `interval` to dump and send (table too
  large for `tx_rate`), rounds are delayed, not dropped: entries are still
  refreshed, just less often. `element_timeout` of `3 x interval` tolerates one
  delayed round; `refresh_overrun` counting up means `tx_rate` is too low for
  the table.
- Our copies are refreshed by our own dump, so the time between two refreshes
  of a copy is up to `interval` plus the duration of a round (`refresh_ms`),
  and it must stay below `element_timeout`. The copies phase walks every copy
  each round (up to `max_copies`); with the defaults (90 s against 30 s plus
  a round of a few seconds) the margin is wide, but watch `refresh_ms` on
  slow hardware under load.
- The kernel-side event filter checks at most 20 client prefixes. With more, all
  IPv6 events of the configured protocols reach user space and are filtered
  there.
- The per-tuple table has 131072 slots (10 MiB) with a 64-slot probe window.
  When a tuple's window is full of live tuples one of them is evicted
  (`rx_evictions`) and misses a refresh until the next round re-learns it; a
  full window stays full, so the same tuples can miss again, and three misses
  in a row expire the copy, which is then re-created. Measured and simulated:
  100k announced flows settle after a few dozen evictions in the first round,
  110k after a few hundred, 120k keep evicting about a hundred per round. Never
  a leak; for more flows raise `RX_TABLE_SIZE` in `flowsync.h`.

## Flow offloading

fw4 `flow_offloading` (software) and `flow_offloading_hw` (hardware) move a
flow into the nf_flowtable after a few packets; from then on its packets
bypass conntrack. The entry keeps `IPS_OFFLOAD` (and `IPS_HW_OFFLOAD`) while
the flow is in the flowtable, the kernel keeps it alive (about a day for UDP
and established TCP), and a dump shows no timeout for it. The flowtable lets a
flow go after `nf_flowtable_udp_timeout` / `nf_flowtable_tcp_timeout` (30 s)
without a packet, and hands the entry back with the protocol's timeout minus
the flowtable timeout (6.12 and 6.18; 6.18 gives the full timeout only to a
flow torn down for another reason, and CLOSE to a closing TCP flow).

- **Copies:** an offloaded copy carries traffic, so it is announced every
  round while it is offloaded, and it is not refreshed (the kernel keeps it
  alive). When the flowtable lets go, its first sighting is no evidence, so it
  is not announced once more for a packet already accounted for. Gauge
  `copies_offloaded`.
- **Natives:** a symmetric flow can be offloaded before conntrack sets
  `ASSURED` (UDP: from its second packet); conntrack then never sees the packet
  that would set it. A fourth dump phase selects natives that are offloaded
  and `SEEN_REPLY` but not `ASSURED`, so they are announced like the others.
- Keep `nf_flowtable_*_timeout` at or below `interval`, so that "offloaded"
  still means "a packet since the last round". `nf_conntrack_acct` is not
  needed.

## TCP

TCP is synced with the same soft-state model as UDP (`list proto 'tcp'`, on
by default). The policy, wire format, dump requests, loop prevention and
the whole TX path are protocol-agnostic; the only TCP-specific code is the
create, which adds `CTA_PROTOINFO_TCP` with state `ESTABLISHED` and a per-ct
`be_liberal` flag on both directions. This makes each injected entry
self-sufficient, so the global `net.netfilter.nf_conntrack_tcp_be_liberal=1`
sysctl is not required; setting it anyway only adds tolerance for the gateways'
own native half-visible TCP entries and is harmless.

What conntrack makes of an asymmetric TCP connection, and why it still works:

- The forward gateway sees the SYN and gets a `SYN_SENT` entry; it never sees
  the SYN/ACK, so the client's following packets are `INVALID` to it (no state
  change, no refresh) and it forwards them anyway (fw4 does not drop invalid
  forward traffic by default). The entry is announced while it exists
  (`nf_conntrack_tcp_timeout_syn_sent`, 120 s); when it expires, the next client
  packet is picked up mid-stream as `ESTABLISHED` (`nf_conntrack_tcp_loose`) and
  announced again.
- The reply gateway holds the copy in `ESTABLISHED` with `be_liberal`; the
  SYN/ACK and the server's data are accepted as established, the copy turns
  `ASSURED` and is announced from its traffic. If the forward gateway has lost
  its native entry in between, it receives the flow back as a copy and the
  client's packets keep that copy alive.
- A SYN/ACK that arrives before the copy exists passes the gateways'
  stateless ACK accept (within its budget) and is picked up like any server
  segment (below); beyond the budget it is rejected with a reset to the
  server, and the client's SYN retransmission starts over. Without such a
  rule it is dropped and retransmitted.
- A server segment with ACK that arrives before the copy exists passes the
  gateways' stateless ACK accept (below) and is picked up by the kernel as a
  connection server -> client. The copy replaces it at the next announcement
  (see "RX"), and the server's segments are established from then on.
- An idle connection keeps its state on the reply gateway: once the copy saw
  the server's traffic it is not cut back to `element_timeout` by refreshes.
  It lives on the timeout the tracker gives it: established, as long as the
  server sent no data the reply gateway saw unacknowledged, otherwise
  `tcp_timeout_unacknowledged` (300 s, see "Known limits"). The forward
  gateway's entry ends after `tcp_timeout_unacknowledged` without packets;
  the client's next packet is picked up again.
- If the reply path moves onto the forward gateway while its entry is still
  `SYN_SENT` (the SYN/ACK took another gateway), the entry is promoted to
  `ESTABLISHED` by the next round instead of dying 120 s after the SYN (see
  "TX").

There is deliberately no close propagation. A closed connection's peer copies
expire within `element_timeout` plus one `interval` once nothing announces
them (a copy that carried the connection's traffic lives on its own TCP
timeout, as a native entry would). The alternative
(subscribing to UPDATE events and shortening peer entries on RST/FIN) was
considered and left out: on asymmetric paths each gateway sees only one
direction, so the signal is unreliable, and the gain over a 90 s soft expiry is
small.

Keep the gateways' rate-limited stateless accept for IPv6 TCP segments with
ACK or RST. It covers the gaps flowsync cannot close: the reply that beats
the first announcement, and the moments between a lost table and the
peers' resync round. Without it every such segment is rejected and the server
gets a reset. With it those segments pass within its budget, and the copy
takes over at the next announcement; beyond the budget they are rejected
just the same (see "Known limits").

## Source layout and tests

`src/`, plain C on libmnl, built by `src/Makefile`. libnetfilter_conntrack is
used only to build the BPF filter for the event socket.

| file | content |
|---|---|
| `flowsync.h` | constants, `struct config`/`flow`/`ct_entry`/`rx_ent`, counters, gauges, prototypes |
| `util.c` | logging, time, address and prefix helpers |
| `policy.c` | `wanted()`, `is_copy()`, `copy_live()`: the rules applied on TX and RX |
| `config.c` | command line options (named after the UCI options) |
| `ctnl.c` | ctnetlink message parsing and building |
| `wire.c` | the binary record format |
| `udp.c` | UDP socket, peers, datagram assembly and fan-out |
| `tx.c` | conntrack NEW and DESTROY events (with the kernel filter), the four-phase streaming refresh, promotion of stuck TCP natives |
| `resync.c` | heartbeats, resync requests, peer liveness |
| `inject.c` | conntrack injection: batches, error attribution, re-create after ENOENT, replacement of reversed entries |
| `rx.c` | datagram handling, the per-tuple table (dedup, ownership, held refreshes) and the copy limit |
| `status.c` | counters, gauges, status file, `status` command |
| `main.c` | main loop and subcommands |
| `test_flowsync.c` | unit test: prefixes, policy, wire format, ctnetlink messages, RX table, injection bookkeeping (`make test`) |

`test/` holds the integration tests, documented in [test/README.md](test/README.md)
(topology, framework, every scenario, known issues): `test/fstest`, a small
Python framework (stdlib only, no root) that runs the real daemon on two to
four gateways in unprivileged network namespaces, with forwarded traffic
between a client and a server namespace, across combinations of per-gateway
profiles (flow offloading, stateless ACK rule).

    make -C src test                                  # unit test
    make -C src itest                                 # integration tests, default matrix (~3 min)
    make -C src itest S="resync tcp_idle"             # some scenarios
    make -C src itest P="g3 off=01 ack=-" S=tcp_idle  # one combination, named as printed
    make -C src check                                 # both

Cross-compiling against an OpenWrt staging dir and running the unit test under
qemu-user:

    export STAGING_DIR=<openwrt>/staging_dir/target-<arch>_musl
    export PATH=<openwrt>/staging_dir/toolchain-<arch>_gcc-*_musl/bin:$PATH
    make -C src CC=<arch>-openwrt-linux-musl-gcc PKG_CONFIG=false \
        CFLAGS="-I$STAGING_DIR/usr/include" LDFLAGS="-L$STAGING_DIR/usr/lib" \
        LDLIBS="-lnetfilter_conntrack -lnfnetlink -lmnl" \
        TEST_RUNNER="qemu-<arch> -L <toolchain dir> -E LD_LIBRARY_PATH=$STAGING_DIR/usr/lib" \
        all test
