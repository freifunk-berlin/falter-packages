// SPDX-License-Identifier: GPL-2.0-only
/* TX: conntrack NEW events and the streaming refresh dumps */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>
#include <libmnl/libmnl.h>
#include <linux/netlink.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/netfilter/nf_conntrack_common.h>
#include <libnetfilter_conntrack/libnetfilter_conntrack.h>

#include "flowsync.h"

/* libnetfilter_conntrack silently drops IPv6 filter entries beyond this */
#define EV_FILTER_MAX_IPV6	20

static struct mnl_socket *ev_nl;
/* NEW events were lost since the last round started (ENOBUFS) */
static bool ev_lost;

/* refresh: one dump socket, one round of requests, a small queue */
static struct mnl_socket *dump_nl;
static unsigned int dump_portid;
static uint32_t dump_seq;
static bool round_running;
static int cur_proto, cur_phase;
static uint64_t round_start, round_progress;
static uint32_t round_start_s, round_no;
static uint64_t round_entries, round_copies, round_copies_live, round_copies_offloaded;
/*
 * Dump chunk generations. The kernel generates a dump's first chunk in the
 * request's sendto and every further one in the recv of the previous chunk
 * (netlink_recvmsg), so the chunk a recv returns shows the table as it was at
 * the previous generation. dump_gen counts generations, chunk_gen is the one
 * of the chunk being parsed. A DESTROY of a copy read at dump_gen >= chunk_gen
 * may have happened after the chunk was generated, which then still shows it.
 */
static uint32_t dump_gen = 1, chunk_gen;

static struct flow queue[Q_SIZE];
static size_t q_len, q_pos;
static double tokens;
static uint64_t tokens_ts;

/*
 * The dump phases of a round, per configured protocol. Native entries (our
 * mark bit clear) are announced while they exist; the kernel-side status
 * filters leave out only the class that never announces, native entries that
 * are SEEN_REPLY but not yet ASSURED. Our copies (mark bit set) are dumped
 * whatever their status, to re-learn ownership and to find the ones with
 * traffic.
 */
static const struct {
	uint32_t status, status_mask;
	bool copies;
} phases[] = {
	{ 0, IPS_SEEN_REPLY, false },		/* native, replies take another gateway */
	{ IPS_ASSURED, IPS_ASSURED, false },	/* native, symmetric */
	/* native, symmetric, offloaded before conntrack set ASSURED: its packets
	 * bypass conntrack now, so it stays SEEN_REPLY without ASSURED and the two
	 * phases above miss it */
	{ IPS_OFFLOAD | IPS_SEEN_REPLY, IPS_OFFLOAD | IPS_SEEN_REPLY | IPS_ASSURED, false },
	{ 0, 0, true },				/* our copies */
};
#define N_PHASES (sizeof(phases) / sizeof(phases[0]))

/*
 * Each phase is a walk over the whole conntrack table with softirqs off
 * until a chunk is full, so phases that cannot find anything are skipped.
 * The offloaded-native phase finds nothing for TCP, which nft offloads only
 * once ASSURED (phase 1 has those), and nothing without the flowtable: it
 * runs for UDP while the nf_flow_table module is loaded or once any entry
 * was seen offloaded.
 */
static bool offload_seen;

static bool phase_wanted(int proto, unsigned int phase)
{
	if (!(phases[phase].status & IPS_OFFLOAD))
		return true;
	if (proto == IPPROTO_TCP)
		return false;
	return offload_seen || access("/sys/module/nf_flow_table", F_OK) == 0;
}

/*
 * Loop prevention through conntrack status. Injected copies carry SEEN_REPLY
 * from the start, so both the UDP and the TCP tracker set ASSURED on the first
 * packet they see in either direction. A copy on a gateway that never sees
 * the flow stays SEEN_REPLY without ASSURED and is never announced:
 *
 *   !SEEN_REPLY          native entry, replies take another gateway
 *   SEEN_REPLY, ASSURED  native symmetric entry, or a copy with traffic
 *   SEEN_REPLY only      copy without traffic: silent, expires
 */
static bool announce_ok(uint32_t status)
{
	/* an offloaded entry carries traffic, ASSURED or not (see phases[]) */
	return !(status & IPS_SEEN_REPLY) || (status & (IPS_ASSURED | IPS_OFFLOAD));
}

/*
 * Kernel-side BPF filter on the event socket (netlink applies sk_filter in
 * the broadcast path): only events of the configured protocols, only IPv6,
 * and only clients inside the configured prefixes reach user space. The
 * builder ANDs the attribute types and ORs the entries of one type; an event
 * without the attribute of a type passes that type. IPv6 events have no IPv4
 * address, so a negative IPv4 filter covering 0.0.0.0/1 and 128.0.0.0/1
 * drops exactly the IPv4 events. exclude, exclude_dst and ports stay in
 * wanted(). Addresses and masks are host order words.
 */
void ev_filter(int fd, bool copies)
{
	struct nfct_filter *filter;
	struct nfct_filter_ipv4 v4[2] = {
		{ .addr = 0x00000000, .mask = 0x80000000 },
		{ .addr = 0x80000000, .mask = 0x80000000 },
	};
	struct nfct_filter_ipv6 v6;
	unsigned int i, k, n6 = 0, np = 0;
	uint32_t w;
	int p;

	filter = nfct_filter_create();
	if (!filter) {
		logmsg(LOG_WARNING, "conntrack events: no memory for the kernel filter");
		return;
	}
	for (p = 0; p < 256; p++) {
		if (!cfg.proto[p])
			continue;
		nfct_filter_add_attr_u32(filter, NFCT_FILTER_L4PROTO, p);
		np++;
	}
	/*
	 * DESTROY: only our copies. NEW: everything but our copies. Each copy we
	 * inject comes back as a NEW event; while a resync answer re-creates
	 * thousands of them, those echoes would fill the socket and overrun it,
	 * and the kernel then drops the NEW events of real flows with them. The
	 * filter runs before the receive buffer is charged. An event without
	 * CTA_MARK counts as mark 0. The callbacks check the mark again in case
	 * the filter could not be attached.
	 */
	{
		struct nfct_filter_dump_mark m = { .val = cfg.ct_mark, .mask = cfg.ct_mark_mask };

		if (!copies)
			nfct_filter_set_logic(filter, NFCT_FILTER_MARK, NFCT_FILTER_LOGIC_NEGATIVE);
		nfct_filter_add_attr(filter, NFCT_FILTER_MARK, &m);
	}
	nfct_filter_set_logic(filter, NFCT_FILTER_SRC_IPV4, NFCT_FILTER_LOGIC_NEGATIVE);
	nfct_filter_add_attr(filter, NFCT_FILTER_SRC_IPV4, &v4[0]);
	nfct_filter_add_attr(filter, NFCT_FILTER_SRC_IPV4, &v4[1]);
	if (cfg.prefix.n && cfg.prefix.n <= EV_FILTER_MAX_IPV6) {
		for (i = 0; i < cfg.prefix.n; i++) {
			const struct prefix *pf = &cfg.prefix.p[i];
			unsigned int len = pf->len;

			for (k = 0; k < 4; k++) {
				memcpy(&w, &pf->addr.s6_addr[4 * k], 4);
				v6.addr[k] = ntohl(w);
				if (len >= 32)
					v6.mask[k] = 0xffffffff;
				else if (len)
					v6.mask[k] = 0xffffffffu << (32 - len);
				else
					v6.mask[k] = 0;
				len = len >= 32 ? len - 32 : 0;
			}
			nfct_filter_add_attr(filter, NFCT_FILTER_SRC_IPV6, &v6);
		}
		n6 = cfg.prefix.n;
	}
	if (nfct_filter_attach(fd, filter) < 0)
		logmsg(LOG_WARNING, "conntrack %s events: kernel filter: %s (all events reach "
		       "user space)", copies ? "destroy" : "new", strerror(errno));
	else
		logmsg(LOG_NOTICE, "conntrack %s events: kernel filter: %u protocols, IPv6 only, "
		       "%u client prefixes%s%s", copies ? "destroy" : "new", np, n6,
		       copies ? ", our mark" : ", not our mark",
		       !n6 && cfg.prefix.n ? " (more than 20 prefixes, filtered in user space)" : "");
	nfct_filter_destroy(filter);
}

static void ev_close(void)
{
	if (ev_nl)
		mnl_socket_close(ev_nl);
	ev_nl = NULL;
}

int events_fd(void)
{
	return ev_nl ? mnl_socket_get_fd(ev_nl) : -1;
}

int ev_open(void)
{
	int grp = NFNLGRP_CONNTRACK_NEW, fd;

	ev_close();
	ev_nl = mnl_socket_open(NETLINK_NETFILTER);
	if (!ev_nl)
		goto err;
	if (mnl_socket_bind(ev_nl, 0, MNL_SOCKET_AUTOPID) < 0)
		goto err;
	fd = mnl_socket_get_fd(ev_nl);
	ev_filter(fd, false);
	if (mnl_socket_setsockopt(ev_nl, NETLINK_ADD_MEMBERSHIP, &grp, sizeof(grp)) < 0)
		goto err;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	set_rcvbuf(fd, "conntrack events", cfg.rcvbuf);
	return 0;
err:
	logmsg(LOG_ERR, "conntrack event subscription: %s", strerror(errno));
	ev_close();
	return -1;
}

static int ev_cb(const struct nlmsghdr *nlh, void *data)
{
	struct ct_entry c;
	struct rx_ent *e;

	(void)data;
	cnt.ev_recv++;
	if ((nlh->nlmsg_type & 0xff) != IPCTNL_MSG_CT_NEW)
		return MNL_CB_OK;
	/* created by our own injection; packet-path events carry portid 0 */
	if (nlh->nlmsg_pid && nlh->nlmsg_pid == inject_portid()) {
		cnt.ev_own++;
		return MNL_CB_OK;
	}
	if (ct_parse(nlh, &c) != 0)
		return MNL_CB_OK;
	/* a packet created an entry for a tuple we believe to hold a copy of:
	 * the copy is gone (flushed, evicted, expired) and this is a native now.
	 * Peers' announcements are then creates with EXCL again (EEXIST leaves
	 * the native alone), not noted for a copy that does not exist. */
	if (!is_copy(c.mark) && (e = rx_find(&c.f)) && e->own) {
		rx_disown(e);
		cnt.rx_own_lost++;
	}
	if (!is_copy(c.mark) && announce_ok(c.status) && wanted(&c.f)) {
		flow_dbg("event", &c.f);
		dgram_add(&c.f);
		cnt.tx_events++;
	}
	return MNL_CB_OK;
}

void handle_events(void)
{
	static char buf[NL_BUF_SIZE] __attribute__((aligned(8)));
	static uint64_t last_log;
	ssize_t n;
	int i;

	for (i = 0; ev_nl && i < DRAIN_MAX; i++) {
		n = mnl_socket_recvfrom(ev_nl, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			if (errno == ENOBUFS) {
				/* overrun: the kernel drops new events until we have
				 * drained the queue; what is queued is intact and the
				 * next refresh covers the gap. Reopening would throw
				 * the queue away. */
				cnt.ev_overruns++;
				ev_lost = true;
				if (log_ok(&last_log))
					logmsg(LOG_WARNING, "conntrack events: overrun, events lost "
					       "(raise rcvbuf)");
				continue;
			}
			logmsg(LOG_WARNING, "conntrack events: %s, resubscribing",
			       strerror(errno));
			ev_lost = true;		/* the queue goes with the socket */
			ev_open();
			break;
		}
		mnl_cb_run(buf, n, 0, 0, ev_cb, NULL);
	}
	/* no batching across reads: the reply may already be on its way. A
	 * peer whose send buffer was full missed these: a round repairs it */
	if (!dgram_flush())
		ev_lost = true;
}

/*
 * DESTROY events of our copies. Any of them ends our ownership, so that the
 * next announcement creates the copy again instead of being noted for a copy
 * that is gone, and makes dump chunks generated before it stale for that copy
 * (gone_gen). A DESTROY event carries CTA_TIMEOUT only if the entry had time
 * left (ctnetlink_dump_timeout skips zero), so a copy destroyed with time
 * left was flushed, deleted or evicted, never expired: then ask the peers for
 * a round now instead of waiting for their next one.
 */
static struct mnl_socket *ds_nl;

int destroy_fd(void)
{
	return ds_nl ? mnl_socket_get_fd(ds_nl) : -1;
}

static void destroy_close(void)
{
	if (ds_nl)
		mnl_socket_close(ds_nl);
	ds_nl = NULL;
}

int destroy_open(void)
{
	int grp = NFNLGRP_CONNTRACK_DESTROY, fd;

	destroy_close();
	ds_nl = mnl_socket_open(NETLINK_NETFILTER);
	if (!ds_nl || mnl_socket_bind(ds_nl, 0, MNL_SOCKET_AUTOPID) < 0)
		goto err;
	fd = mnl_socket_get_fd(ds_nl);
	ev_filter(fd, true);
	if (mnl_socket_setsockopt(ds_nl, NETLINK_ADD_MEMBERSHIP, &grp, sizeof(grp)) < 0)
		goto err;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	set_rcvbuf(fd, "conntrack destroy events", cfg.rcvbuf);
	return 0;
err:
	logmsg(LOG_ERR, "conntrack destroy event subscription: %s", strerror(errno));
	destroy_close();
	return -1;
}

static int destroy_cb(const struct nlmsghdr *nlh, void *data)
{
	struct ct_entry c;
	struct rx_ent *e;

	(void)data;
	if ((nlh->nlmsg_type & 0xff) != IPCTNL_MSG_CT_DELETE)
		return MNL_CB_OK;
	if (ct_parse(nlh, &c) != 0 || !is_copy(c.mark) || !wanted(&c.f))
		return MNL_CB_OK;
	/* one of our copies is gone, expired or not */
	e = rx_find(&c.f);
	if (e) {
		rx_disown(e);		/* the next announcement creates it, */
		e->t_inject = 0;	/* however soon after the last create */
		e->gone_gen = dump_gen;
	}
	if (!c.timeout)
		return MNL_CB_OK;	/* expired */
	cnt.copies_lost++;
	resync_request();
	return MNL_CB_OK;
}

/*
 * DESTROY events were lost (a flush or an eviction storm overran the socket,
 * or it had to be reopened): which copies are gone is unknown. The kernel
 * goes on dropping events until the queue is empty, so the episode ends
 * only when a read finds it empty (ds_overrun).
 */
static bool ds_overrun;

static void destroys_lost(void)
{
	static uint64_t last_log;

	cnt.ds_overruns++;
	ds_overrun = true;
	if (log_ok(&last_log))
		logmsg(LOG_WARNING, "conntrack destroy events: overrun, ownership of all copies "
		       "dropped (raise rcvbuf)");
	rx_disown_all();
	resync_request();
}

void handle_destroy(void)
{
	static char buf[NL_BUF_SIZE] __attribute__((aligned(8)));
	bool drained = !ds_nl;
	ssize_t n;
	int i;

	for (i = 0; ds_nl && i < DRAIN_MAX; i++) {
		n = mnl_socket_recvfrom(ds_nl, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				drained = true;
				break;
			}
			if (errno == EINTR)
				continue;
			if (errno == ENOBUFS) {
				destroys_lost();
				continue;
			}
			logmsg(LOG_WARNING, "conntrack destroy events: %s, resubscribing",
			       strerror(errno));
			destroys_lost();
			destroy_open();
			drained = true;		/* a fresh socket holds nothing */
			break;
		}
		mnl_cb_run(buf, n, 0, 0, destroy_cb, NULL);
	}
	/* stopped at DRAIN_MAX: the last read may have emptied the queue, and
	 * then poll never reports it again; look. Any read, the peek too,
	 * returns and clears an overrun that happened meanwhile. */
	while (!drained && ds_nl) {
		if (recv(mnl_socket_get_fd(ds_nl), buf, 1, MSG_PEEK | MSG_DONTWAIT) >= 0)
			break;			/* more to read: poll reports it */
		if (errno == ENOBUFS)
			destroys_lost();
		else if (errno == EAGAIN || errno == EWOULDBLOCK)
			drained = true;
		else if (errno != EINTR)
			break;
	}
	/* the queue is empty, which ends an overrun. Ownership asserted while the
	 * kernel was still dropping events may be of copies already gone, so it
	 * is dropped again; the resync request goes out now and its answers
	 * re-create what is missing (EEXIST for what exists, the dump re-learns) */
	if (drained && ds_overrun) {
		ds_overrun = false;
		rx_disown_all();
	}
	/* a resync request waits until every queued DESTROY is read */
	resync_destroys_pending(!drained);
}

/*
 * Refresh. Every interval the IPv6 table is dumped per configured protocol in
 * four kernel-filtered phases (see phases[]). The dump is read one chunk at a
 * time from the poll loop and only while the queue has room for a chunk: the
 * kernel generates the next chunk on our recv, so the table walk is paced by
 * tx_rate and never stalls the loop. A round whose entries are not all sent
 * when the next interval comes is not restarted and nothing is dropped: the
 * next round is delayed until the queue is empty (refresh_overrun). Only a
 * dump that stops delivering chunks is aborted and started over.
 */

static void queue_push(const struct flow *f)
{
	if (q_len == Q_SIZE) {
		cnt.tx_refresh_dropped++;
		return;
	}
	queue[q_len++] = *f;
	round_entries++;
}

static int dump_cb(const struct nlmsghdr *nlh, void *data)
{
	struct ct_entry c;
	struct rx_ent *e;
	uint32_t now, prev_seen;
	bool live;

	(void)data;
	cnt.tx_scanned++;
	if (ct_parse(nlh, &c) != 0 || !wanted(&c.f))
		return MNL_CB_OK;
	if (c.status & IPS_OFFLOAD)
		offload_seen = true;
	if (!phases[cur_phase].copies) {
		/* native: announced while it exists. Peers never refresh it
		 * (EXCL), so it lives on packets alone. */
		if (!is_copy(c.mark) && announce_ok(c.status))
			queue_push(&c.f);
		return MNL_CB_OK;
	}
	if (!is_copy(c.mark))
		return MNL_CB_OK;
	/* one of our copies: announce it only with evidence of a packet since the
	 * previous round, then (re)learn that we own it and remember what we saw */
	now = now_s();
	e = rx_find(&c.f);
	/* the kernel generated this chunk before the copy's DESTROY that we have
	 * read already: what it shows is gone and must not be owned again */
	if (e && e->gone_gen && e->gone_gen >= chunk_gen)
		return MNL_CB_OK;
	if (c.status & IPS_OFFLOAD) {
		/*
		 * Offloaded: the flow is in the flowtable, its packets bypass
		 * conntrack and the dump shows no timeout. It stays offloaded only
		 * while packets keep coming (nf_flowtable_*_timeout, 30 s), so that is
		 * the evidence. The kernel keeps the entry alive itself (a day,
		 * topped up), so it is not refreshed. The baseline is cleared: when
		 * the flowtable lets go, the first sighting of the protocol's
		 * timeout is no evidence (that packet is already accounted for).
		 */
		e = rx_seed(&c.f, round_no, now);
		e->seen_at = 0;
		e->seen_timeout = 0;
		round_copies++;
		round_copies_offloaded++;
		round_copies_live++;
		cnt.tx_copies++;
		queue_push(&c.f);
		return MNL_CB_OK;
	}
	live = e && copy_live(c.status, c.timeout, e->seen_timeout, e->seen_at, now);
	prev_seen = e ? e->seen_at : 0;
	e = rx_seed(&c.f, round_no, now);
	switch (refresh_due(e, c.timeout, prev_seen, now)) {
	case REFRESH_YES:
		flow_dbg("refresh", &c.f);
		inj_add(&c.f, e, INJ_REFRESH);
		/* t_inject stays: it dedups creates of the same tuple from several
		 * peers, a refresh must not make a create after a flush a "duplicate" */
		e->seen_at = now;
		e->seen_timeout = cfg.element_timeout;
		break;
	case REFRESH_HELD:
		cnt.inject_held++;
		/* fall through */
	default:
		e->seen_at = now;
		e->seen_timeout = c.timeout;
	}
	round_copies++;
	if (live) {
		round_copies_live++;
		cnt.tx_copies++;
		queue_push(&c.f);
	}
	return MNL_CB_OK;
}

/* NEW events were lost and no round has started since: the main loop pulls
 * the next round forward instead of waiting up to a whole interval */
bool events_lost(void)
{
	return ev_lost;
}

/* the subscription was down and is back: its events are lost */
void events_reopened(void)
{
	ev_lost = true;
}

/* first configured protocol after "from", or -1 */
static int next_proto(int from)
{
	int p;

	for (p = from + 1; p < 256; p++)
		if (cfg.proto[p])
			return p;
	return -1;
}

static int dump_open(void)
{
	int fd;

	dump_nl = mnl_socket_open(NETLINK_NETFILTER);
	if (!dump_nl)
		return -1;
	if (mnl_socket_bind(dump_nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		mnl_socket_close(dump_nl);
		dump_nl = NULL;
		return -1;
	}
	dump_portid = mnl_socket_get_portid(dump_nl);
	fd = mnl_socket_get_fd(dump_nl);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	/* the kernel generates the next chunk only while the socket holds less
	 * than half its receive buffer; two chunks must fit */
	set_rcvbuf(fd, "conntrack dump", DUMP_RCVBUF);
	return 0;
}

static int dump_send(void)
{
	char buf[256] __attribute__((aligned(8)));
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);

	ct_build_dump(nlh, cur_proto, phases[cur_phase].status, phases[cur_phase].status_mask,
		      phases[cur_phase].copies ? cfg.ct_mark : 0, cfg.ct_mark_mask);
	if (!++dump_seq)
		dump_seq = 1;
	nlh->nlmsg_seq = dump_seq;
	if (mnl_socket_sendto(dump_nl, nlh, nlh->nlmsg_len) < 0)
		return -1;
	dump_gen++;		/* the first chunk */
	return 0;
}

/*
 * Abort: always close the socket. A dump in progress keeps the socket busy
 * (netlink_dump_start returns EBUSY) and leftover chunks would fail the
 * sequence check.
 */
void refresh_close(void)
{
	if (dump_nl)
		mnl_socket_close(dump_nl);
	dump_nl = NULL;
	round_running = false;
	gauge.refresh_running = false;
}

static void refresh_fail(const char *what)
{
	static uint64_t last_log;

	cnt.refresh_errors++;
	if (log_ok(&last_log))
		logmsg(LOG_WARNING, "refresh: %s: %s", what, strerror(errno));
	refresh_close();
}

int refresh_fd(void)
{
	return dump_nl ? mnl_socket_get_fd(dump_nl) : -1;
}

/* read the next chunk only while a full one fits into the queue */
bool refresh_wants_read(void)
{
	return round_running && dump_nl && Q_SIZE - (q_len - q_pos) >= Q_CHUNK_RESERVE;
}

/* queued entries, and a held datagram counts as one */
size_t refresh_pending(void)
{
	return q_len - q_pos + (dgram_held() ? 1 : 0);
}

/* 1: a round started, 0: the previous one is still being dumped or sent,
 * -1: it could not start */
int refresh_start(void)
{
	static uint64_t last_log;
	uint64_t now = mono_ms();

	if (round_running && now - round_progress > 2 * cfg.interval * 1000) {
		/* no chunk for two intervals: the dump is stuck, not slow */
		cnt.refresh_errors++;
		logmsg(LOG_WARNING, "refresh: no dump progress for %llu s, restarting the round",
		       (unsigned long long)((now - round_progress) / 1000));
		refresh_close();
	}
	if (round_running || refresh_pending()) {
		/* the previous round is still being dumped or sent: let it finish,
		 * the next one starts as soon as its queue is empty */
		cnt.refresh_overrun++;
		if (log_ok(&last_log))
			logmsg(LOG_WARNING, "refresh: previous round not finished after %lu s "
			       "(%zu entries still to send), next one delayed (raise tx_rate)",
			       cfg.interval, refresh_pending());
		return 0;
	}
	if (!dump_nl && dump_open() < 0) {
		refresh_fail("netlink socket");
		return -1;
	}
	cur_proto = next_proto(-1);
	if (cur_proto < 0)
		return -1;
	cur_phase = 0;
	round_start = round_progress = now;
	round_start_s = now_s();
	round_no++;
	round_entries = round_copies = round_copies_live = round_copies_offloaded = 0;
	round_running = true;
	gauge.refresh_running = true;
	if (dump_send() < 0) {
		refresh_fail("dump request");
		return -1;
	}
	ev_lost = false;	/* this round covers what the events missed */
	return 1;
}

void handle_refresh(void)
{
	static char buf[NL_BUF_SIZE] __attribute__((aligned(8)));
	ssize_t n;
	int ret, err;

	if (!round_running || !dump_nl)
		return;
	/* make room for a whole chunk at the end of the queue */
	if (q_pos == q_len) {
		q_pos = q_len = 0;
	} else if (q_pos) {
		memmove(queue, queue + q_pos, (q_len - q_pos) * sizeof(*queue));
		q_len -= q_pos;
		q_pos = 0;
	}

	n = mnl_socket_recvfrom(dump_nl, buf, sizeof(buf));
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			refresh_fail("receive");
		return;
	}
	round_progress = mono_ms();
	chunk_gen = dump_gen;
	dump_gen++;		/* this recv generated the next chunk */
	ret = mnl_cb_run(buf, n, dump_seq, dump_portid, dump_cb, NULL);
	err = errno;
	inj_flush();	/* the refreshes this chunk decided */
	if (ret < 0) {
		/* an error message, or NLM_F_DUMP_INTR (ctnetlink does not set it
		 * today), where libmnl stops before the rest of the chunk and its
		 * NLMSG_DONE: either way the round is started over */
		errno = err;
		refresh_fail("dump");
		return;
	}
	if (ret > 0)
		return;

	/* NLMSG_DONE: next request, or the round is complete */
	do {
		if ((unsigned int)++cur_phase == N_PHASES) {
			cur_phase = 0;
			cur_proto = next_proto(cur_proto);
		}
	} while (cur_proto >= 0 && !phase_wanted(cur_proto, cur_phase));
	if (cur_proto >= 0) {
		if (dump_send() < 0)
			refresh_fail("dump request");
		return;
	}
	round_running = false;
	gauge.refresh_running = false;
	gauge.refresh_ms = mono_ms() - round_start;
	gauge.refresh_entries = round_entries;
	gauge.copies = round_copies;
	gauge.copies_live = round_copies_live;
	gauge.copies_offloaded = round_copies_offloaded;
	rx_sweep(round_no, round_start_s);
	cnt.refresh_rounds++;
	DBG("refresh: round done, %llu entries (%llu of %llu copies with traffic) in %llu ms",
	    (unsigned long long)round_entries, (unsigned long long)round_copies_live,
	    (unsigned long long)round_copies, (unsigned long long)gauge.refresh_ms);
}

static double burst_size(void)
{
	double burst = cfg.tx_rate / 20.0;

	return burst < 1 ? 1 : burst;
}

/* poll timeout while refresh records are pending: one burst worth of time */
int refresh_pace_ms(void)
{
	int ms = (int)(1000.0 * burst_size() / cfg.tx_rate);

	return ms < 10 ? 10 : ms;
}

/* send queued refresh entries at tx_rate datagrams per second (per peer) */
void refresh_tick(void)
{
	uint64_t now = mono_ms();
	double burst = burst_size();
	unsigned int i;

	/* a round held back by its own send queue is slow, not stuck */
	if (round_running && !refresh_wants_read())
		round_progress = now;

	tokens += (now - tokens_ts) * (double)cfg.tx_rate / 1000.0;
	tokens_ts = now;
	if (tokens > burst)
		tokens = burst;

	/* a datagram some peer's send buffer had no room for goes first; the
	 * queue waits until it is out (a slower round, nothing dropped) */
	if (dgram_held() && !dgram_resend())
		return;
	while (tokens >= 1 && q_pos < q_len) {
		for (i = 0; i < cfg.batch_lines && q_pos < q_len; i++) {
			dgram_add(&queue[q_pos++]);
			cnt.tx_refresh++;
		}
		dgram_send(true);
		tokens -= 1;
		if (dgram_held())
			break;
	}
	if (q_pos == q_len)
		q_pos = q_len = 0;
}
