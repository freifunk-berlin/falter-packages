// SPDX-License-Identifier: GPL-2.0-only
/*
 * RX: datagram handling and the per-tuple table.
 *
 * The table remembers, per announced tuple, when it was last announced and
 * injected, and whether the conntrack entry for it is a copy created by this
 * daemon. Only such copies are refreshed; everything else is created with
 * NLM_F_EXCL, so an entry this gateway holds natively is never touched by a
 * peer's announcement (EEXIST). Ownership is set tentatively on create, taken
 * back by the injection error, and confirmed or revoked by the refresh dump
 * of our marked entries every round (rx_seed/rx_sweep), which also heals
 * daemon restarts, slot evictions and external flushes.
 *
 * Open addressing with a short linear probe; a slot whose last announcement
 * is older than element_timeout describes an expired copy and is reusable.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>

#include "flowsync.h"

static struct rx_ent *table;
static unsigned int mask;

static uint32_t flow_hash(const struct flow *f)
{
	uint32_t h = 2166136261u;
	unsigned int i;

#define MIX(b) do { h ^= (uint8_t)(b); h *= 16777619u; } while (0)
	for (i = 0; i < 16; i++) {
		MIX(f->c.s6_addr[i]);
		MIX(f->s.s6_addr[i]);
	}
	MIX(f->cport);
	MIX(f->cport >> 8);
	MIX(f->sport);
	MIX(f->sport >> 8);
	MIX(f->proto);
#undef MIX
	return h;
}

int rx_init(unsigned int size)
{
	table = calloc(size, sizeof(*table));
	if (!table) {
		logmsg(LOG_ERR, "out of memory");
		return -1;
	}
	mask = size - 1;
	return 0;
}

/* the slot describing f, or NULL */
struct rx_ent *rx_find(const struct flow *f)
{
	uint32_t h = flow_hash(f);
	unsigned int i;

	for (i = 0; i < RX_PROBES; i++) {
		struct rx_ent *e = &table[(h + i) & mask];

		if (!e->t_rx)
			return NULL;
		if (flow_eq(&e->f, f))
			return e;
	}
	return NULL;
}

/* a fresh slot for f: the first free or expired one in the probe window,
 * otherwise the oldest, evicting a live tuple (counted) */
struct rx_ent *rx_insert(const struct flow *f, uint32_t now)
{
	uint32_t h = flow_hash(f);
	struct rx_ent *e, *best = NULL;
	unsigned int i;

	for (i = 0; i < RX_PROBES; i++) {
		e = &table[(h + i) & mask];
		if (!e->t_rx || now - e->t_rx > cfg.element_timeout) {
			best = e;
			goto take;
		}
		if (!best || e->t_rx < best->t_rx)
			best = e;
	}
	cnt.rx_evictions++;
take:
	memset(best, 0, sizeof(*best));
	best->f = *f;
	best->t_rx = now;
	return best;
}

/*
 * What to do with an announcement for the tuple in e. For a copy of ours it
 * is only noted (t_ann): the copy is refreshed by our own dump, which knows
 * its remaining timeout (refresh_due). Anything else is created with EXCL;
 * the same tuple from any peer within interval/2 is created once. own is
 * tentative on create and taken back by inj_account() when the kernel says
 * the entry is not ours.
 */
enum rx_class rx_classify(struct rx_ent *e, uint32_t now)
{
	e->t_rx = now;
	if (e->own) {
		e->t_ann = now;
		return RX_NOTED;
	}
	if (e->t_inject && now - e->t_inject < cfg.interval / 2)
		return RX_DUP;
	e->t_inject = now;
	e->t_ann = now;
	/* the create sets the copy's timeout to exactly element_timeout: the
	 * baseline for the evidence check (copy_live) */
	e->seen_at = now;
	e->seen_timeout = cfg.element_timeout;
	rx_own(e, now);
	return RX_CREATE;
}

/*
 * The copy is ours from now on: tentatively after a create, confirmed when
 * our dump sees it. own_since is what the sweep goes by, not t_inject, which
 * a DESTROY or an overrun zeroes so that the next create is not filtered as a
 * duplicate. A create also ends what a DESTROY said about the copy (gone_gen).
 */
void rx_own(struct rx_ent *e, uint32_t now)
{
	e->own = true;
	e->own_since = now;
	e->gone_gen = 0;
}

/*
 * Whether our dump refreshes a copy it just saw with `remaining` seconds
 * left; prev_seen is when the dump (or our create/refresh) last looked at it.
 *
 * Only while a peer announces it: an announcement since that last look. One
 * lost datagram costs one refresh, element_timeout (3 x interval) covers it.
 * Without announcements nothing refreshes the copy and it expires; after a
 * restart (no announcement known yet) the resync request brings them.
 *
 * A refresh sets the timeout to exactly element_timeout, it does not extend
 * it. A copy that saw traffic has the protocol's own, longer timeout (TCP
 * established: hours); a refresh would cut that short and an idle connection
 * would lose its state here as soon as the forward gateway falls silent. So a
 * copy with more than element_timeout plus one interval left (until the next
 * dump looks again) is held, not refreshed: it lives like a native entry,
 * and the evidence check works on its decaying timeout as before. A copy
 * created or refreshed less than interval/2 ago is left for the next round.
 *
 * Deciding this in the dump, not when the announcement arrives, matters: only
 * the dump knows the current timeout, a packet may have raised it since. One
 * race remains, because a refresh can only set the timeout: a packet that
 * arrives in the milliseconds between the dump reading a copy and the refresh
 * being applied has its timeout cut back to element_timeout.
 */
enum refresh_do refresh_due(const struct rx_ent *e, uint32_t remaining, uint32_t prev_seen,
			    uint32_t now)
{
	(void)now;
	if (!e->t_ann || e->t_ann < prev_seen)
		return REFRESH_NO;
	if (remaining > cfg.element_timeout + cfg.interval)
		return REFRESH_HELD;
	/* still fresh (created or refreshed less than interval/2 ago): the next
	 * round is soon enough. A refresh now would only race the copy's first
	 * packets, which raise the timeout between our dump and our refresh.
	 * The kernel rounds the timeout down (a copy created a moment ago shows
	 * element_timeout - 1), and interval/2 is taken exactly (doubled). */
	if (2 * ((uint64_t)remaining + 1) + cfg.interval > 2 * (uint64_t)cfg.element_timeout)
		return REFRESH_NO;
	return REFRESH_YES;
}

/* the refresh dump saw a marked entry: it is a copy of ours, learn or confirm
 * that (after a restart or an eviction the slot may be new). The copy exists,
 * so the slot is live: a copy that lives on its own traffic and that no peer
 * announces any more must not look expired to rx_insert, or its evidence
 * baseline is lost with the slot. */
struct rx_ent *rx_seed(const struct flow *f, uint32_t round, uint32_t now)
{
	struct rx_ent *e = rx_find(f);

	if (!e) {
		e = rx_insert(f, now);
		/* old enough not to look like a duplicate, young enough not to be
		 * swept before the next round confirms it */
		e->t_inject = now > cfg.interval ? now - cfg.interval : 1;
	}
	e->t_rx = now;
	rx_own(e, now);
	e->seen_round = round;
	return e;
}

/* inject_created and copies_lost when the running round started, and when
 * the last complete one did; with the copies that round counted, the base of
 * copies_estimate */
static uint64_t created_at_start, created_base, lost_at_start, lost_base, copies_base;

void rx_round_start(void)
{
	created_at_start = cnt.inject_created;
	lost_at_start = cnt.copies_lost;
}

/*
 * Our copies in the table now, estimated: what the last complete dump counted
 * plus what we created since that round started, minus what was destroyed
 * early since (flush, eviction). Plain expiries are not subtracted and creates
 * during the round may be counted twice, so this errs high until the next
 * round corrects it.
 */
uint64_t copies_estimate(void)
{
	uint64_t up = copies_base + (cnt.inject_created > created_base ?
				     cnt.inject_created - created_base : 0);
	uint64_t down = cnt.copies_lost > lost_base ? cnt.copies_lost - lost_base : 0;

	return up > down ? up - down : 0;
}

/*
 * DESTROY events of our copies were lost (the socket overran: a flush or an
 * eviction storm). We no longer know which copies exist, so none is assumed:
 * every announcement becomes a create with EXCL, a copy that still exists
 * answers EEXIST and the next dump learns it again. The copy count restarts
 * from what is created from now on.
 */
void rx_disown_all(void)
{
	unsigned int i;

	for (i = 0; i <= mask; i++) {
		table[i].own = false;
		table[i].t_inject = 0;
	}
	copies_base = 0;
	created_base = created_at_start = cnt.inject_created;
	lost_base = lost_at_start = cnt.copies_lost;
}

/* after a complete round: a copy owned since before the round started that
 * the dump did not see is gone (expired, flushed, evicted); stop treating it
 * as ours, so that the next announcement creates it again */
void rx_sweep(uint32_t round, uint32_t round_start)
{
	unsigned int i;
	uint64_t owned = 0;

	created_base = created_at_start;
	lost_base = lost_at_start;
	copies_base = gauge.copies;

	for (i = 0; i <= mask; i++) {
		struct rx_ent *e = &table[i];

		if (!e->own)
			continue;
		if (e->seen_round != round && e->own_since < round_start)
			e->own = false;
		else
			owned++;
	}
	gauge.owned = owned;
}

/*
 * Copy limit. Announcements are not authenticated, so anybody who can send
 * from a peer's address can make us create entries; without a bound a few
 * Mbit/s of forged records fill nf_conntrack_max and the gateway drops new
 * flows of its own clients. Beyond the limit no new copy is created and no
 * table slot is taken for it (rx_limited); refreshes of our copies and the
 * re-creates after ENOENT go on. A forger can then degrade the sync, not the
 * gateway.
 *
 * Default: a quarter of nf_conntrack_max, and at most three quarters of the
 * per-tuple table (beyond that it evicts live tuples, see RX_PROBES).
 */
void rx_limit_init(void)
{
	unsigned long ct_max, rx_max = RX_TABLE_SIZE / 4 * 3;

	if (cfg.max_copies) {
		if (cfg.max_copies > rx_max)
			logmsg(LOG_WARNING, "max_copies %lu exceeds what the per-tuple table holds "
			       "without evictions (%lu)", cfg.max_copies, rx_max);
	} else {
		cfg.max_copies = rx_max;
		if (!read_sysctl("/proc/sys/net/netfilter/nf_conntrack_max", &ct_max) &&
		    ct_max / 4 < rx_max)
			cfg.max_copies = ct_max / 4;
	}
	logmsg(LOG_NOTICE, "copy limit %lu", cfg.max_copies);
}

bool rx_limit_reached(void)
{
	return copies_estimate() >= cfg.max_copies;
}

static void rx_record(const struct flow *f)
{
	static uint64_t last_log;
	uint32_t now = now_s();
	struct rx_ent *e;

	if (!wanted(f)) {
		cnt.rx_policy++;
		flow_dbg("rx: rejected by policy", f);
		return;
	}
	e = rx_find(f);
	/* a create (tuple not known as our copy) beyond the limit */
	if ((!e || !e->own) && rx_limit_reached()) {
		cnt.rx_limited++;
		if (log_ok(&last_log))
			logmsg(LOG_WARNING, "copy limit %lu reached, new copies refused (forged "
			       "announcements, or raise max_copies)", cfg.max_copies);
		return;
	}
	if (!e)
		e = rx_insert(f, now);
	switch (rx_classify(e, now)) {
	case RX_DUP:
		cnt.rx_dup++;
		return;
	case RX_NOTED:
		cnt.rx_records++;	/* our copy: refreshed by our dump */
		return;
	default:
		cnt.rx_records++;
		flow_dbg("rx: create", f);
		inj_add(f, e, INJ_CREATE);
		return;
	}
}

void handle_rx(void)
{
	static uint8_t buf[65536];
	static uint64_t last_log;
	struct sockaddr_in6 from;
	socklen_t fromlen;
	char abuf[INET6_ADDRSTRLEN];
	struct flow f;
	unsigned int count, r;
	uint8_t flags;
	ssize_t n;
	int i, p, rc;

	/* drain a bounded burst, then inject it in one netlink batch */
	for (i = 0; i < DRAIN_MAX; i++) {
		fromlen = sizeof(from);
		n = recvfrom(udp_fd, buf, sizeof(buf), MSG_DONTWAIT,
			     (struct sockaddr *)&from, &fromlen);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			if (log_ok(&last_log))
				logmsg(LOG_WARNING, "udp receive: %s", strerror(errno));
			break;
		}
		p = from.sin6_family == AF_INET6 ? peer_index(&from.sin6_addr) : -1;
		if (p < 0) {
			cnt.rx_datagrams++;
			cnt.rx_bad_peer++;
			DBG("rx: %zd bytes from non-peer %s", n,
			    addr_str(&from.sin6_addr, abuf, sizeof(abuf)));
			continue;
		}
		peer_rx[p]++;
		peer_last[p] = now_s();
		rc = wire_check(buf, n, &count, &flags);
		if (rc == PARSE_OK && (flags & WIRE_F_RESYNC))
			resync_from(p);
		if (rc == PARSE_OK && !count) {
			cnt.rx_control++;	/* heartbeat or resync request */
			continue;
		}
		cnt.rx_datagrams++;
		switch (rc) {
		case PARSE_OK:
			break;
		case PARSE_VERSION:
			cnt.rx_version++;
			DBG("rx: unknown wire version %u from %s", buf[0],
			    addr_str(&from.sin6_addr, abuf, sizeof(abuf)));
			continue;
		default:
			cnt.rx_parse++;
			DBG("rx: malformed datagram of %zd bytes from %s", n,
			    addr_str(&from.sin6_addr, abuf, sizeof(abuf)));
			continue;
		}
		resync_answered(p);
		for (r = 0; r < count; r++) {
			if (wire_get(buf + WIRE_HDR_LEN + r * WIRE_REC_LEN, &f) != PARSE_OK) {
				cnt.rx_parse++;
				continue;
			}
			rx_record(&f);
		}
	}
	inj_flush();
}
