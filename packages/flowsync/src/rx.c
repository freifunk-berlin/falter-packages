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
#include <ifaddrs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#include "flowsync.h"

static struct rx_ent *table;
static unsigned int mask;

/*
 * Both hashes take tuples that anybody on the mesh can choose (and, without
 * authentication, records anybody can forge): seeded at startup, so that
 * nobody can aim at one probe window.
 */
static uint32_t seed;

/*
 * Our copies, counted: gauge.owned is the number of slots that own one, and
 * clients[] counts them per client prefix (client_prefix_len, the copy
 * limits). Both follow every change of own (rx_own, rx_disown) and are
 * recounted from the table by each sweep, which also drops prefixes without
 * copies. Open addressing on the prefix (0: empty, no client of a synced
 * prefix has it); a prefix whose window is full is not counted and so not
 * limited.
 */
#define CLIENTS		(1 << 16)
#define CLIENT_PROBES	64
static struct client {
	uint64_t pfx;
	uint32_t n;
} clients[CLIENTS];

/* the client's prefix, the first client_prefix_len bits, as a number */
static uint64_t client_prefix(const struct flow *f)
{
	uint64_t p = 0;
	unsigned int i;

	for (i = 0; i < 8; i++)
		p = p << 8 | f->c.s6_addr[i];
	return p & ~0ull << (64 - cfg.client_prefix_len);
}

static uint32_t *client_count(const struct flow *f, bool add)
{
	uint64_t p = client_prefix(f);
	uint32_t h;
	unsigned int i;

	if (!p)
		return NULL;		/* the empty mark, not a client */
	h = (uint32_t)(((p ^ seed) ^ (p >> 31)) * 0x9e3779b97f4a7c15ull >> 40);
	for (i = 0; i < CLIENT_PROBES; i++) {
		struct client *c = &clients[(h + i) & (CLIENTS - 1)];

		if (c->pfx == p)
			return &c->n;
		if (!c->pfx) {
			if (!add)
				return NULL;
			c->pfx = p;
			return &c->n;
		}
	}
	return NULL;
}

static uint32_t flow_hash(const struct flow *f)
{
	uint32_t h = 2166136261u ^ seed;
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
	gauge.owned = 0;
	memset(clients, 0, sizeof(clients));
	if (getrandom(&seed, sizeof(seed), GRND_NONBLOCK) != sizeof(seed))
		seed = (uint32_t)mono_ms() ^ (uint32_t)getpid() << 16;
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
	rx_disown(best);	/* an evicted or expired slot may still own a copy */
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
	uint32_t *n;

	if (!e->own) {
		e->own = true;
		gauge.owned++;
		n = client_count(&e->f, true);
		if (n)
			(*n)++;
	}
	e->own_since = now;
	e->gone_gen = 0;
}

/* the copy is not ours (any more), or not known to exist */
void rx_disown(struct rx_ent *e)
{
	uint32_t *n;

	if (!e->own)
		return;
	e->own = false;
	if (gauge.owned)
		gauge.owned--;
	n = client_count(&e->f, false);
	if (n && *n)
		(*n)--;
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

/*
 * DESTROY events of our copies were lost (the socket overran: a flush or an
 * eviction storm). We no longer know which copies exist, so none is assumed:
 * every announcement becomes a create with EXCL, a copy that still exists
 * answers EEXIST and the next dump learns it again. The copy count restarts
 * from there.
 */
void rx_disown_all(void)
{
	unsigned int i;

	for (i = 0; i <= mask; i++) {
		table[i].own = false;
		table[i].t_inject = 0;
	}
	gauge.owned = 0;
	memset(clients, 0, sizeof(clients));
}

/* after a complete round: a copy owned since before the round started that
 * the dump did not see is gone (expired, flushed, evicted); stop treating it
 * as ours, so that the next announcement creates it again. The copy counts
 * are recounted from the table. */
void rx_sweep(uint32_t round, uint32_t round_start)
{
	unsigned int i;
	uint32_t *n;

	gauge.owned = 0;
	memset(clients, 0, sizeof(clients));
	for (i = 0; i <= mask; i++) {
		struct rx_ent *e = &table[i];

		if (!e->own)
			continue;
		if (e->seen_round != round && e->own_since < round_start) {
			e->own = false;
			continue;
		}
		gauge.owned++;
		n = client_count(&e->f, true);
		if (n)
			(*n)++;
	}
}

/*
 * Copy limit. Announcements are not authenticated, so anybody who can send
 * from a peer's address can make us create entries; without a bound a few
 * Mbit/s of forged records fill nf_conntrack_max and the gateway drops new
 * flows of its own clients. Beyond the limit no new copy is created and no
 * table slot is taken for it (rx_limited); refreshes of our copies and the
 * re-creates after ENOENT go on. A forger can then degrade the sync, not the
 * gateway. The same happens without any forging when one client opens flows
 * by the thousand (a scanner, P2P): every peer would hold a copy of each, and
 * the pool would be gone for everybody else. So each client prefix may hold
 * only max_copies_per_client of them (rx_limited_client), when that is set.
 * The prefix is client_prefix_len bits long: a /64 is one client network, and
 * a /56 one Freifunk location, whose host can source from any of its /64s.
 *
 * Default: a quarter of nf_conntrack_max (0: unlimited), and at most three
 * quarters of the per-tuple table (beyond that it evicts live tuples, see
 * RX_PROBES); no limit per client.
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
		    ct_max && ct_max / 4 < rx_max)
			cfg.max_copies = ct_max / 4 ? ct_max / 4 : 1;
	}
	if (cfg.max_copies_client)
		logmsg(LOG_NOTICE, "copy limit %lu, %lu per client /%lu", cfg.max_copies,
		       cfg.max_copies_client, cfg.client_prefix_len);
	else
		logmsg(LOG_NOTICE, "copy limit %lu, no limit per client", cfg.max_copies);
}

/* whether a new copy of f may be created (not for our own copies: refreshes
 * and re-creates go on beyond the limits) */
bool rx_admit(const struct flow *f)
{
	static uint64_t last_log, last_log_client;
	char buf[INET6_ADDRSTRLEN];
	struct in6_addr p;
	uint32_t *n;

	if (gauge.owned >= cfg.max_copies) {
		cnt.rx_limited++;
		if (log_ok(&last_log))
			logmsg(LOG_WARNING, "copy limit %lu reached, new copies refused (raise "
			       "max_copies, or forged announcements)", cfg.max_copies);
		return false;
	}
	n = cfg.max_copies_client ? client_count(f, false) : NULL;
	if (n && *n >= cfg.max_copies_client) {
		cnt.rx_limited_client++;
		if (log_ok(&last_log_client)) {
			uint64_t pfx = client_prefix(f);
			unsigned int i;

			memset(&p, 0, sizeof(p));
			for (i = 0; i < 8; i++)
				p.s6_addr[i] = pfx >> (56 - 8 * i);
			logmsg(LOG_WARNING, "client %s/%lu holds %u copies, more refused "
			       "(max_copies_per_client)", addr_str(&p, buf, sizeof(buf)),
			       cfg.client_prefix_len, *n);
		}
		return false;
	}
	return true;
}

/*
 * This gateway's own IPv6 addresses, re-read every interval. A flow to one
 * of them is never on an asymmetric path through another gateway, and a copy
 * of it would make that traffic established here, whatever interface it
 * comes in on and whatever the zone rules say: such a record is refused.
 * (Networks behind the gateway are not known here: exclude_dst covers them.)
 */
#define MAX_LOCAL 512
static struct in6_addr local_addr[MAX_LOCAL];
static unsigned int n_local;

void rx_local_refresh(void)
{
	static uint64_t last_log;
	struct ifaddrs *ifa, *i;
	bool full = false;

	if (getifaddrs(&ifa))
		return;
	n_local = 0;
	for (i = ifa; i; i = i->ifa_next) {
		const struct in6_addr *a;

		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET6)
			continue;
		a = &((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr;
		/* never a server of a synced flow (wanted() needs a routable one) */
		if (IN6_IS_ADDR_LINKLOCAL(a) || IN6_IS_ADDR_LOOPBACK(a))
			continue;
		if (n_local == MAX_LOCAL) {
			full = true;
			break;
		}
		local_addr[n_local++] = *a;
	}
	freeifaddrs(ifa);
	if (full && log_ok(&last_log))
		logmsg(LOG_WARNING, "more than %d local IPv6 addresses: copies of flows to the "
		       "others are not refused", MAX_LOCAL);
}

static bool is_local(const struct in6_addr *a)
{
	unsigned int i;

	for (i = 0; i < n_local; i++)
		if (IN6_ARE_ADDR_EQUAL(a, &local_addr[i]))
			return true;
	return false;
}

static void rx_record(const struct flow *f)
{
	uint32_t now = now_s();
	struct rx_ent *e;

	if (!wanted(f) || is_local(&f->s)) {
		cnt.rx_policy++;
		flow_dbg("rx: rejected by policy", f);
		return;
	}
	e = rx_find(f);
	/* a create (tuple not known as our copy) beyond a limit */
	if ((!e || !e->own) && !rx_admit(f))
		return;
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
	char abuf[INET6_ADDRSTRLEN];
	struct flow f;
	unsigned int count, r;
	uint8_t flags;
	bool forged;
	ssize_t n;
	int i, p, rc;

	/* drain a bounded burst, then inject it in one netlink batch */
	for (i = 0; i < DRAIN_MAX; i++) {
		n = udp_recv(buf, sizeof(buf), &from, &forged);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			if (log_ok(&last_log))
				logmsg(LOG_WARNING, "udp receive: %s", strerror(errno));
			break;
		}
		/* a v4-mapped source that came as IPv6 is nobody's (udp_recv) */
		p = from.sin6_family == AF_INET6 && !forged ? peer_index(&from.sin6_addr) : -1;
		if (p < 0) {
			static uint64_t last_bad;

			cnt.rx_datagrams++;
			cnt.rx_bad_peer++;
			/* a peer whose source address is not the one configured
			 * here (bind_address unset on a multi-homed gateway) looks
			 * exactly like this: say who it was */
			if (log_ok(&last_bad))
				logmsg(LOG_NOTICE, "rx: %zd bytes from %s%s, not a peer", n,
				       addr_str(&from.sin6_addr, abuf, sizeof(abuf)),
				       forged ? " (v4-mapped over IPv6)" : "");
			continue;
		}
		rc = wire_check(buf, n, &count, &flags);
		/* liveness from well-formed datagrams only: garbage with a peer's
		 * address must not hide that the peer is gone */
		if (rc == PARSE_OK) {
			peer_rx[p]++;
			peer_last[p] = now_s();
		}
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
