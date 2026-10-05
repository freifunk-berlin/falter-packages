// SPDX-License-Identifier: GPL-2.0-only
/*
 * flowsync - scoped conntrack flow announcer for active-active gateways
 *
 * TX: announce new (and, every interval, all) wanted IPv6 conntrack entries
 *     to all peers as binary UDP records.
 * RX: inject announced flows into the local conntrack table with a timeout
 *     and a ct mark bit, so fw4's "ct state established" accepts the reply.
 *
 * Soft state only: no acks, no sequence numbers, no DESTROY propagation.
 *
 * Three rules keep stale flows from propagating forever:
 *  1. a peer never refreshes an entry this gateway did not create (NLM_F_EXCL
 *     for everything but our own copies), so native entries live on packets
 *     alone;
 *  2. a native entry is announced while it exists, a copy only while its
 *     remaining timeout exceeds element_timeout, which only a packet can
 *     cause (a refresh sets exactly element_timeout);
 *  3. ownership of copies is re-learned from the kernel every refresh round
 *     (the mark), so restarts, evictions and flushes heal themselves.
 *
 * util.c     logging, time, address and prefix helpers
 * policy.c   the filter applied identically on TX and RX
 * config.c   command line options (named after the UCI options)
 * ctnl.c     ctnetlink message parsing and building (libmnl)
 * wire.c     the binary record format
 * udp.c      UDP socket, peers, datagram assembly and fan-out
 * tx.c       conntrack events and the streaming refresh dumps
 * inject.c   conntrack injection over netlink
 * rx.c       datagram handling and the per-tuple table
 * resync.c   heartbeats, resync requests, peer liveness
 * status.c   counters, gauges, status file
 * main.c     main loop and subcommands
 */

#ifndef FLOWSYNC_H
#define FLOWSYNC_H

#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <syslog.h>

struct nlmsghdr;

/* wire format, see wire.c */
#define WIRE_VERSION	1
#define WIRE_HDR_LEN	4
#define WIRE_REC_LEN	40
#define WIRE_F_RESYNC	0x01	/* header flag: send me a round now */
#define MAX_DGRAM	1400
/* records per datagram accepted on receive and allowed by batch_lines */
#define WIRE_MAX_RECORDS ((MAX_DGRAM - WIRE_HDR_LEN) / WIRE_REC_LEN)	/* 34 */
/*
 * Default batch_lines: 4 + 30 x 40 = 1204 bytes of payload, 1252 bytes as an
 * IPv6/UDP packet, 1232 as IPv4: fits the IPv6 minimum MTU of 1280, so a path
 * that drops large packets loses no refresh. 34 records need 1420 bytes.
 */
#define MAX_BATCH	30

#define MAX_PEERS	32
#define MAX_PREFIXES	256
/*
 * Per-tuple RX table: slots (power of two, 80 bytes each) and the linear
 * probing window. A tuple whose window is full of live tuples evicts one,
 * which then misses a refresh; three misses in a row expire the copy. A full
 * window stays full, so a short window churns the same tuples every round
 * (measured: 8 probes lose copies at 100k flows, 32 still a couple). With 64
 * probes 131072 slots settle at 100k flows after the first round; 120k flows
 * keep churning. Raise RX_TABLE_SIZE for more.
 */
#define RX_TABLE_SIZE	(1 << 17)
#define RX_PROBES	64
/* injection: netlink sequence -> record ring for error reports, and the
 * re-creates after ENOENT per flush: one receive pass can carry DRAIN_MAX
 * full datagrams, and after a flush every record in it may be a refresh of a
 * copy that is gone, so the retry list holds a whole pass */
#define INJ_RING	4096
#define DRAIN_MAX	64
#define INJ_RETRY	(DRAIN_MAX * WIRE_MAX_RECORDS)
/*
 * Refresh queue. A dump chunk is at most 32 KiB (kernel cap) and an IPv6 UDP
 * entry serializes to about 200 bytes, so a chunk holds at most ~170 entries.
 * A chunk is only read when Q_CHUNK_RESERVE entries are free.
 */
#define Q_SIZE		2048
#define Q_CHUNK_RESERVE	512
#define INJECT_BATCH	(32 * 1024)
#define NL_BUF_SIZE	(32 * 1024)
#define DUMP_RCVBUF	(256 * 1024)
#define LOG_INTERVAL_MS	10000
#define EARLY_ROUND_MS	1000	/* delay of the round pulled forward after lost events */
#define STATUS_FILE	"/var/run/flowsync.status"

struct prefix {
	struct in6_addr addr;
	unsigned int len;
};

struct prefix_list {
	struct prefix p[MAX_PREFIXES];
	unsigned int n;
};

struct config {
	bool debug;
	bool bind_set;
	struct in6_addr bind;
	char ifname[IFNAMSIZ];		/* the sync socket's device, "" for any */
	unsigned long port, interval, element_timeout, batch_lines, tx_rate, rcvbuf;
	unsigned long ct_mark, ct_mark_mask;
	unsigned long max_copies;	/* 0: derived at startup, see rx_limit_init() */
	unsigned long max_copies_client;	/* per client /64; 0: derived */
	bool proto[256];
	unsigned int n_proto;
	uint8_t skip_port[65536 / 8];
	unsigned int n_skip_port;
	struct in6_addr peer[MAX_PEERS];
	unsigned int n_peer;
	struct prefix_list prefix, exclude, exclude_dst;
};

/* original tuple, client -> server; ports in host byte order */
struct flow {
	struct in6_addr c, s;
	uint16_t cport, sport;
	uint8_t proto;
};

/* what the TX side reads from a conntrack entry */
struct ct_entry {
	struct flow f;
	uint32_t status;	/* IPS_* */
	uint32_t mark;
	uint32_t timeout;	/* remaining seconds */
	uint32_t id;		/* CTA_ID, network order as the kernel sent it */
};

/*
 * Per-tuple RX state. t_rx is the last announcement received for the tuple
 * (0: empty slot), t_inject the last create we sent for it (the duplicate
 * filter's clock, 0: none to filter against). own says that the entry in the
 * kernel table is a copy created by us; only then is a refresh sent,
 * everything else is a create with NLM_F_EXCL. own is set (rx_own) on create,
 * tentatively, and by our dump of marked entries; it is cleared by the
 * injection error (EEXIST, ENOENT) and by any DESTROY of the copy, and revoked
 * by the sweep after a complete round that started after own_since and did
 * not see the copy (seen_round).
 */
struct rx_ent {
	struct flow f;
	uint32_t t_rx;
	uint32_t t_ann;		/* last announcement of the tuple from a peer */
	uint32_t t_inject;
	uint32_t seen_round;
	uint32_t seen_at;	/* when the dump last saw the copy ... */
	uint32_t seen_timeout;	/* ... and its remaining timeout then */
	uint32_t checked_at;	/* when an EEXIST for it was last looked up */
	uint32_t own_since;	/* when own was last set */
	uint32_t gone_gen;	/* dump generation at the copy's last DESTROY, 0: none */
	bool own;
};

enum rx_class { RX_DUP, RX_NOTED, RX_CREATE };
enum refresh_do { REFRESH_NO, REFRESH_HELD, REFRESH_YES };
enum inj_kind { INJ_CREATE, INJ_REFRESH, INJ_CHECK, INJ_DELETE };
enum inj_acct { ACCT_OK, ACCT_RETRY, ACCT_ERROR };

#define COUNTERS(X) \
	X(tx_events) X(tx_scanned) X(tx_refresh) X(tx_copies) X(tx_datagrams) X(tx_errors) \
	X(tx_refresh_dropped) X(tx_control) X(tx_resync) X(refresh_rounds) X(refresh_overrun) X(refresh_errors) \
	X(ev_recv) X(ev_own) X(ev_overruns) X(ds_overruns) \
	X(rx_datagrams) X(rx_control) X(rx_resync) X(rx_records) X(rx_bad_peer) X(rx_policy) \
	X(rx_parse) X(rx_version) X(rx_dup) X(rx_evictions) X(rx_own_lost) X(rx_limited) X(rx_limited_client) \
	X(inject_created) X(inject_refreshed) X(inject_held) X(inject_exists) X(inject_replaced) X(inject_gone) X(inject_errors) \
	X(copies_lost)

struct counters {
#define X(name) uint64_t name;
	COUNTERS(X)
#undef X
};

/* snapshots, not counters: written to the status file and the interval log */
struct gauges {
	uint64_t refresh_ms;		/* wall time of the last complete refresh round */
	uint64_t refresh_entries;	/* entries queued by the last complete round */
	uint64_t loop_max_ms;		/* longest handler run between two polls, per interval */
	uint64_t copies;		/* our copies in the table, last complete round */
	uint64_t copies_live;		/* of these, with traffic since their last refresh */
	uint64_t copies_offloaded;	/* of these, in the flowtable (fw4 flow offloading) */
	uint64_t owned;			/* our copies: slots that own one (live, the limit) */
	bool refresh_running;
};

extern struct config cfg;
extern struct counters cnt;
extern struct gauges gauge;
extern int udp_fd;
extern const char *status_path;

/* util.c */
#define DBG(...) logmsg(LOG_DEBUG, __VA_ARGS__)
void log_open(bool daemon);
void logmsg(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
const char *flow_str(const struct flow *f, char *buf, size_t len);
void flow_dbg(const char *what, const struct flow *f);
uint64_t mono_ms(void);
uint32_t now_s(void);
bool log_ok(uint64_t *last);
int parse_addr(const char *s, struct in6_addr *a);
const char *addr_str(const struct in6_addr *a, char *buf, size_t len);
int parse_prefix(const char *s, struct prefix *p);
bool in_list(const struct in6_addr *a, const struct prefix_list *l);
int parse_port(const char *s, uint16_t *port);
int read_sysctl(const char *path, unsigned long *v);

/* policy.c */
const char *proto_name(uint8_t proto);
int proto_num(const char *s);
bool skip_port(unsigned int port);
bool wanted(const struct flow *f);
bool is_copy(uint32_t mark);
bool copy_live(uint32_t status, uint32_t remaining, uint32_t prev_remaining, uint32_t prev_at,
	       uint32_t now);

/* config.c */
int parse_args(int argc, char **argv);
void usage(FILE *out);

/* ctnl.c */
int ct_parse(const struct nlmsghdr *nlh, struct ct_entry *e);
void ct_build_new(struct nlmsghdr *nlh, const struct flow *f, bool create);
void ct_build_get(struct nlmsghdr *nlh, const struct flow *f);
void ct_build_delete(struct nlmsghdr *nlh, const struct flow *orig, uint32_t id);
void flow_reverse(struct flow *r, const struct flow *f);
bool flow_eq(const struct flow *a, const struct flow *b);
void ct_build_dump(struct nlmsghdr *nlh, uint8_t proto, uint32_t status, uint32_t status_mask,
		   uint32_t mark, uint32_t mark_mask);

/* wire.c */
enum { PARSE_OK, PARSE_ERR, PARSE_VERSION };
void wire_put_hdr(uint8_t *dst, unsigned int count, uint8_t flags);
void wire_put(uint8_t *dst, const struct flow *f);
int wire_check(const uint8_t *buf, size_t len, unsigned int *count, uint8_t *flags);
int wire_get(const uint8_t *src, struct flow *f);

/* udp.c */
void set_rcvbuf(int fd, const char *what, unsigned long size);
int udp_open(bool bind_port);
ssize_t udp_recv(uint8_t *buf, size_t len, struct sockaddr_in6 *from, bool *forged);
void peers_init(void);
int peer_index(const struct in6_addr *a);
/* per peer: datagrams received and when the last one came (now_s, 0: never) */
extern uint64_t peer_rx[MAX_PEERS];
extern uint32_t peer_last[MAX_PEERS];
extern uint64_t peer_tx_errors[MAX_PEERS];
void dgram_add(const struct flow *f);
bool dgram_flush(void);
bool dgram_send(bool hold);
bool dgram_held(void);
bool dgram_resend(void);
void dgram_control(uint8_t flags);

/* tx.c */
int ev_open(void);
int events_fd(void);
void handle_events(void);
void ev_filter(int fd, bool copies);
int refresh_fd(void);
bool refresh_wants_read(void);
int refresh_start(void);
void refresh_close(void);
void handle_refresh(void);
void refresh_tick(void);
size_t refresh_pending(void);
bool events_lost(void);
void events_reopened(void);
int refresh_pace_ms(void);

/* inject.c */
int inj_open(void);
int inject_fd(void);
unsigned int inject_portid(void);
void inj_drain(void);
void inj_flush(void);
void inj_add(const struct flow *f, struct rx_ent *e, enum inj_kind kind);
enum inj_acct inj_account(enum inj_kind kind, int err, struct rx_ent *e);

/* rx.c */
int rx_init(unsigned int size);
void handle_rx(void);
struct rx_ent *rx_find(const struct flow *f);
struct rx_ent *rx_insert(const struct flow *f, uint32_t now);
enum rx_class rx_classify(struct rx_ent *e, uint32_t now);
enum refresh_do refresh_due(const struct rx_ent *e, uint32_t remaining, uint32_t prev_seen,
			    uint32_t now);
struct rx_ent *rx_seed(const struct flow *f, uint32_t round, uint32_t now);
void rx_sweep(uint32_t round, uint32_t round_start);
void rx_disown_all(void);
void rx_own(struct rx_ent *e, uint32_t now);
void rx_disown(struct rx_ent *e);
void rx_limit_init(void);
bool rx_admit(const struct flow *f);

/* resync.c */
int destroy_open(void);
int destroy_fd(void);
void handle_destroy(void);
void resync_request(void);
void resync_destroys_pending(bool pending);
void resync_from(int peer);
bool resync_round_wanted(void);
void resync_round_pulled(void);
bool resync_own_round_wanted(void);
void resync_round_started(void);
void resync_tick(void);
void resync_init(void);
void heartbeat(void);

/* status.c */
void log_counters(void);
void write_status(void);
int cmd_status(void);

#endif
