// SPDX-License-Identifier: GPL-2.0-only
/*
 * flowsync - flow tables for active-active gateways without conntrack
 *
 * Forwarded IPv6 bypasses conntrack. Two tc programs on the uplink keep and
 * consult two BPF maps instead (dp.h): the flows this gateway forwarded out
 * (written by the packet path) and the flows the other gateways announced
 * (written by this daemon). A packet from the uplink whose flow is in either
 * gets a mark, and the firewall accepts the mark.
 *
 * TX: announce new (and, every interval, all) wanted local flows to all
 *     peers as binary UDP records.
 * RX: put announced flows into the remote map with element_timeout.
 *
 * Soft state only: no acks, no sequence numbers, no close propagation. Only
 * the local map is announced and only packets write it, so nothing can keep
 * itself alive.
 *
 * util.c     logging, time, address and prefix helpers
 * policy.c   which flows are synced, applied identically on TX and RX
 * config.c   command line options (named after the UCI options)
 * wire.c     the binary record format
 * udp.c      UDP socket, peers, datagram assembly and fan-out
 * dp.c       the datapath: tc programs, maps, new-flow events
 * fw.c       the nftables rules the datapath needs, kept in place
 * tx.c       new-flow events and the refresh rounds
 * rx.c       datagram handling
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
/* refresh queue, how many map entries one walk step reads, and how many
 * steps one loop iteration takes at most */
#define Q_SIZE		4096
#define WALK_BATCH	512
#define WALK_STEPS	16
#define DRAIN_MAX	64
#define LOG_INTERVAL_MS	10000
#define EARLY_ROUND_MS	1000	/* delay of the round pulled forward after lost events */
#define STATUS_FILE	"/var/run/flowsync/status"
#define BPF_OBJECT	"/lib/bpf/flowsync.o"
#define PIN_DIR		"/sys/fs/bpf/flowsync"
#define FW_TABLE	"fw4"

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
	char uplink[IFNAMSIZ];		/* the device the tc programs attach to */
	const char *user;		/* run as this user once everything is open */
	const char *bpf_object, *pin_dir, *fw_table;
	unsigned long port, interval, element_timeout, batch_lines, tx_rate, rcvbuf;
	unsigned long mark;		/* packet mark of accepted packets */
	unsigned long max_flows;	/* local map entries */
	unsigned long max_copies;	/* remote map entries */
	/* lifetime of a local flow after its last packet out, seconds */
	unsigned long t_udp, t_tcp, t_tcp_syn, t_tcp_close, t_other;
	unsigned long resync_rate;	/* tx_rate of a round answering a resync request */
	bool proto[256];
	unsigned int n_proto;
	uint8_t skip_port[65536 / 8];
	unsigned int n_skip_port;
	struct in6_addr peer[MAX_PEERS];
	unsigned int n_peer;
	struct prefix_list prefix, exclude, exclude_dst;
};

/* client -> server; ports in host byte order */
struct flow {
	struct in6_addr c, s;
	uint16_t cport, sport;
	uint8_t proto;
};

#define COUNTERS(X) \
	X(tx_events) X(tx_refresh) X(tx_datagrams) X(tx_errors) X(tx_control) X(tx_resync) \
	X(refresh_rounds) X(refresh_overrun) X(refresh_errors) \
	X(ev_recv) X(ev_overruns) \
	X(rx_datagrams) X(rx_control) X(rx_resync) X(rx_records) X(rx_bad_peer) X(rx_policy) \
	X(rx_parse) X(rx_version) X(rx_limited) X(rx_errors) \
	X(local_expired) X(remote_expired) X(dp_attached) X(fw_repaired)

struct counters {
#define X(name) uint64_t name;
	COUNTERS(X)
#undef X
};

/* snapshots, not counters: written to the status file and the interval log */
struct gauges {
	uint64_t refresh_ms;		/* wall time of the last complete refresh round */
	uint64_t refresh_entries;	/* flows announced by the last complete round */
	uint64_t loop_max_ms;		/* longest handler run between two polls, per interval */
	uint64_t local;			/* live local flows, last complete round */
	uint64_t copies;		/* live remote flows, last complete round */
	bool refresh_running;
	bool attached;			/* the tc programs are on the uplink */
	bool fw_ok;			/* the nftables rules are in place */
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

/* config.c */
int parse_args(int argc, char **argv);
void usage(FILE *out);

/* wire.c */
enum { PARSE_OK, PARSE_ERR, PARSE_VERSION };
void wire_put_hdr(uint8_t *dst, unsigned int count, uint8_t flags);
void wire_put(uint8_t *dst, const struct flow *f);
int wire_check(const uint8_t *buf, size_t len, unsigned int *count, uint8_t *flags);
int wire_get(const uint8_t *src, struct flow *f);

/* udp.c */
void set_rcvbuf(int fd, const char *what, unsigned long size);
int udp_open(bool bind_port);
bool udp_tick(void);
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
unsigned int dgram_control(uint8_t flags);

/* dp.c */
struct fs_stats;
struct dp_walk;
enum dp_map { DP_LOCAL, DP_REMOTE };
/* one entry: for DP_LOCAL the time since its last packet and the lifetime
 * that leaves, for DP_REMOTE the time left; left 0: expired */
struct dp_ent {
	struct flow f;
	uint32_t age, left;
	uint8_t flags;
};
int dp_open(bool load);
bool dp_tick(void);
int dp_detach(void);
int dp_events_fd(void);
void dp_handle_events(void (*cb)(const struct flow *f));
uint32_t dp_now(void);
struct dp_walk *dp_walk_start(enum dp_map which);
int dp_walk_next(struct dp_walk *w, struct dp_ent *out, unsigned int max, bool *done);
void dp_walk_end(struct dp_walk *w);
int dp_delete(enum dp_map which, const struct flow *f);
void dp_remote_add(const struct flow *f);
void dp_remote_flush(void);
int dp_get(enum dp_map which, const struct flow *f, struct dp_ent *out);
int dp_stats(struct fs_stats *sum);

/* fw.c */
int fw_open(void);
int fw_fd(void);
void fw_handle(void);
void fw_tick(void);
void fw_remove(void);

/* tx.c */
void tx_event(const struct flow *f);
void tx_events_done(void);
int refresh_start(void);
void refresh_close(void);
void refresh_tick(void);
void refresh_fast(bool fast);
size_t refresh_pending(void);
bool events_lost(void);
int refresh_pace_ms(void);

/* rx.c */
void handle_rx(void);

/* resync.c */
void resync_request(void);
void resync_from(int peer);
bool resync_round_wanted(void);
void resync_round_pulled(void);
void resync_round_started(void);
void resync_tick(void);
void resync_init(void);
void heartbeat(void);

/* status.c */
void log_counters(void);
void write_status(void);
int cmd_status(void);

#endif
