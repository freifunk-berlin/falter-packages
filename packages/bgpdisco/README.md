# bgpdisco protocol

bgpdisco floods small per-node data through the mesh, using BGP (BIRD) as
transport. Each piece of data is attached to a host route of the node that
owns it, so the data disappears when the node does.

## Transport

- iBGP, AS 65000, one session per babel neighbor, peering over link-local
  (`direct`, bound to the interface).
- Every session is `rr client yes`: each node reflects everything it learns
  to all its other neighbors. CLUSTER_LIST/ORIGINATOR_ID prevent loops.
- Routes live in the dedicated tables `v4_bgpdisco` / `v6_bgpdisco` and are
  never used for forwarding. Next hops are documentation addresses
  (`192.0.2.0`, `2001:db8::`).
- The sessions follow the babel neighbors of bird: bgpdisco compares them
  every `peer_sync_interval` seconds (default 30), every second for half
  a minute after a link came up, and at once when the kernel reports a
  new reachable `fe80::` neighbor on a babel interface.
  A neighbor that babel has lost keeps its session for another
  `peer_removal_delay` seconds (default 300).
- bgpdisco itself is one more iBGP peer of the local bird, on `::1`
  (`bgpdisco_local`, bird is passive, bgpdisco connects). Over this session
  it announces the data of the node and receives the data of all others.
  Its router ID is the one of bird moved into `240.0.0.0/4`
  (`10.248.67.84` -> `250.248.67.84`), it shows up as ORIGINATOR_ID. It only
  has to differ from the ID of the local bird.
- bgpdisco never sends a NOTIFICATION, it just closes the connection: bird
  then keeps the routes and accepts the reconnect at once, instead of
  dropping them and backing off for 60 to 300 seconds. bgpdisco slows down
  by itself: a session that fails within its first minute is retried after
  5, 10, 20 ... up to 300 seconds.

## Announcement

For every local IP that carries data, bgpdisco announces a host route
(`/32` or `/128`) with BGP path attribute **250** (`bytestring`) to bird.
The plugins are asked again every `refresh_local_data_interval` seconds;
what changed is announced or withdrawn.

bgpdisco advertises graceful restart: when it restarts, bird keeps its
routes for `graceful_restart_time` seconds, so the mesh does not see the
restart. If it stays away, the routes are withdrawn after that time.

Attribute value: a JSON array of plugin records, one per plugin:

```
[[<plugin-id>, <data>...], ...]
```

Example for a node `sama` with loopback `10.31.1.1`:

```
10.31.1.1/32  attr 250 = [[0,"lo.sama","sama"]]
```

## Reception

bird sends its best route per IP, and every later change, over the session.
bgpdisco keeps attribute 250 of each host route and regroups the records by
plugin ID:

```
{ <plugin-id>: { <ip>: [<data>...], ... } }
```

Each handler receives the map for its own ID, the local data of the node
included. Handlers run about a second after a change, but at most once per
`handler_min_interval` seconds (default 30), so a burst of changes is one
run. While bird is still sending its table after a (re)connect, and while
the session is down, the handlers keep their last result.

After a reconnect, data that bird does not send again is kept for another
60 seconds after bird reported its table as complete: a bird that has just
restarted does so before its sessions into the mesh are back. The 60 seconds
are not started again by another reconnect, so that data also expires while
the session flaps; in that case it can go earlier than 60 seconds after the
last reconnect.

## Plugin IDs

| ID | Plugin      | Data                                         |
|----|-------------|----------------------------------------------|
| 0  | nameservice | hostnames, without the domain (e.g. `sama`)  |

## Limits and caveats

- **Unauthenticated:** any neighbor can announce anything. Treat received
  data as untrusted input.
- **Size:** the attribute must fit in a BGP UPDATE (4096 bytes).
- **No format version:** the attribute is bare JSON.
- **Duplicates:** if two nodes announce the same IP, the route bird selects
  as best wins. For its own IPs a node always uses its own data.
- **Latency:** remote changes become visible after about a second, or after
  up to `handler_min_interval` when the handlers ran shortly before. Local
  changes are announced after up to one `refresh_local_data_interval`.
