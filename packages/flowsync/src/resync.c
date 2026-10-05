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
 * interval/2, and repeats the request on its next two heartbeats unless every
 * peer has sent records since (a request is a single datagram). A peer
 * honours a request from the same peer at most once per interval/4, and
 * serves requests with at most one extra round per interval/2, whoever asked:
 * a round started meanwhile serves them all, and forged requests from many
 * peer addresses cannot make it run rounds back to back.
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
static uint64_t last_served;		/* when a round last served requests (mono ms) */
static unsigned int repeats;		/* heartbeats that repeat our request */
static uint64_t answered;		/* peers that sent records since we asked */

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

/* a datagram with records came from a peer: it answers our request, if any */
void resync_answered(int peer)
{
	answered |= 1ull << peer;
}

/* main loop: a peer's request is to be served by a round now (at most one
 * such extra round per interval/2; a regular round serves it too) */
bool resync_round_wanted(void)
{
	return round_wanted &&
	       (!last_served || mono_ms() - last_served >= cfg.interval * 1000 / 2);
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
	if (round_wanted)
		last_served = mono_ms();
	round_wanted = false;
	own_round_wanted = false;
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
	request_pending = false;
	last_request = now;
	cnt.tx_resync++;
	dgram_control(WIRE_F_RESYNC);
	own_round_wanted = true;
	repeats = 2;
	answered = 0;
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
	uint64_t all = cfg.n_peer >= 64 ? ~0ull : (1ull << cfg.n_peer) - 1;
	uint8_t flags = 0;
	unsigned int i;
	bool silent;

	/* our request may have been lost: repeat it while some peer has not
	 * answered with records (a peer without flows to announce never does,
	 * hence the bound) */
	if (repeats) {
		repeats--;
		if ((answered & all) != all)
			flags = WIRE_F_RESYNC;
	}
	dgram_control(flags);
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
