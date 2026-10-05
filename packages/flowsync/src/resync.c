// SPDX-License-Identifier: GPL-2.0-only
/*
 * Liveness and loss recovery.
 *
 * Heartbeat: every interval each daemon sends a record-less datagram to all
 * peers, so a peer that falls silent is noticed (status: peer ... age).
 *
 * Resync: a gateway that lost copies asks its peers for a round now instead
 * of waiting up to one interval for their next one. It asks at startup (a
 * restart or reboot: the table may be gone, and refreshes that arrived while
 * the daemon was down are lost) and when one of its copies is destroyed early
 * (see handle_destroy() in tx.c). A requester asks at most once per
 * interval/2; a request that gets lost (a single datagram) costs the wait for
 * the peers' next regular round. A peer honours a request from the same peer at most once
 * per interval/4, and pulls rounds forward for requests at most twice per
 * interval, whoever asked (a budget that refills): a round started meanwhile
 * serves them all, and forged requests from many peer addresses cannot make
 * it run rounds back to back.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <string.h>
#include <syslog.h>

#include "flowsync.h"

static bool request_pending;		/* ask the peers for a round */
static uint64_t last_request;		/* when we last asked (mono ms) */
static bool round_wanted;		/* a peer asked us for a round */
static bool own_round_wanted;		/* we asked: refresh from the answers */
static uint64_t peer_asked[MAX_PEERS];	/* last request honoured, per peer */
static bool peer_silent[MAX_PEERS];
static uint32_t started;		/* now_s() at startup */
static bool destroys_pending;		/* DESTROY events still queued */
static double budget = 2;		/* rounds we may pull forward for requests */
static uint64_t budget_ts;

/* ask the peers for a round as soon as allowed */
void resync_request(void)
{
	request_pending = true;
}

/*
 * The DESTROY socket still holds events (a flush queues one per copy). The
 * request waits until they are all read: an answer that arrives for a copy
 * whose destruction we have not seen yet is taken for a refresh of a copy we
 * own and noted, not re-created.
 */
void resync_destroys_pending(bool pending)
{
	destroys_pending = pending;
}

/* a peer asked for a round */
void resync_from(int peer)
{
	uint64_t now = mono_ms();

	cnt.rx_resync++;
	if (peer_asked[peer] && now - peer_asked[peer] < cfg.interval * 1000 / 4)
		return;
	peer_asked[peer] = now;
	round_wanted = true;
}

/* two rounds per interval, at most two in a row */
static void budget_refill(uint64_t now)
{
	if (budget_ts)
		budget += (double)(now - budget_ts) * 2 / (cfg.interval * 1000.0);
	if (budget > 2)
		budget = 2;
	budget_ts = now;
}

/* main loop: a peer's request is to be served by a round now, if the budget
 * allows; otherwise it waits for the budget or the next regular round */
bool resync_round_wanted(void)
{
	if (!round_wanted)
		return false;
	budget_refill(mono_ms());
	return budget >= 1;
}

/* main loop: it pulls the next round forward for the requests */
void resync_round_pulled(void)
{
	budget -= 1;
}

/* we asked the peers: our copies are refreshed by our own dump, so run one
 * shortly after the answers came in */
bool resync_own_round_wanted(void)
{
	return own_round_wanted;
}

/* a round started: it serves every request that came before it */
void resync_round_started(void)
{
	round_wanted = false;
	own_round_wanted = false;
}

/*
 * The conntrack table is at least 95 % full (looked at once a second). The
 * kernel then evicts entries that are not ASSURED, our idle copies first
 * (early_drop, and the gc worker above 95 %): a copy lost that way is no
 * reason to have every peer re-send its table, the copies it would re-create
 * only evict other entries, the gateway's own new flows among them.
 */
static bool table_pressure(uint64_t now)
{
	static uint64_t checked;
	static bool full;
	static uint64_t last_log;
	unsigned long count, max;

	if (checked && now - checked < 1000)
		return full;
	checked = now;
	full = !read_sysctl("/proc/sys/net/netfilter/nf_conntrack_count", &count) &&
	       !read_sysctl("/proc/sys/net/netfilter/nf_conntrack_max", &max) &&
	       max && count >= max / 20 * 19;
	if (full && log_ok(&last_log))
		logmsg(LOG_WARNING, "conntrack table %lu of %lu: resync request held", count, max);
	return full;
}

/* main loop: send a pending request if the rate allows */
void resync_tick(void)
{
	uint64_t now = mono_ms();

	if (!request_pending || destroys_pending ||
	    (last_request && now - last_request < cfg.interval * 1000 / 2))
		return;
	/* not before our first round has re-learned which copies are ours: the
	 * answers would otherwise arrive as creates, hit EEXIST on our own
	 * surviving copies and refresh nothing */
	if (!cnt.refresh_rounds)
		return;
	/* held, not dropped: it goes out once the table has room again */
	if (table_pressure(now))
		return;
	request_pending = false;
	last_request = now;
	cnt.tx_resync++;
	dgram_control(WIRE_F_RESYNC);
	own_round_wanted = true;
}

void resync_init(void)
{
	started = now_s();
	resync_request();	/* the table may be gone, refreshes were missed */
}

/* every interval: heartbeat, and log peers going silent or coming back */
void heartbeat(void)
{
	char abuf[INET6_ADDRSTRLEN];
	uint32_t now = now_s(), last, limit = 3 * cfg.interval;
	unsigned int i;
	bool silent;

	dgram_control(0);
	for (i = 0; i < cfg.n_peer; i++) {
		last = peer_last[i] ? peer_last[i] : started;
		silent = now - last > limit;
		if (silent && !peer_silent[i])
			logmsg(LOG_WARNING, "peer %s: nothing received for %u s",
			       addr_str(&cfg.peer[i], abuf, sizeof(abuf)), now - last);
		else if (!silent && peer_silent[i])
			logmsg(LOG_NOTICE, "peer %s: back", addr_str(&cfg.peer[i], abuf, sizeof(abuf)));
		peer_silent[i] = silent;
	}
}
