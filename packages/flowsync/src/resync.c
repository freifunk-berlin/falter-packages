// SPDX-License-Identifier: GPL-2.0-only
/*
 * Liveness and loss recovery.
 *
 * Heartbeat: every interval each daemon sends a record-less datagram to all
 * peers, so a peer that falls silent is noticed (status: peer ... age).
 *
 * Resync: a gateway that may have missed announcements asks its peers for a
 * round now instead of waiting up to one interval for their next one. It asks
 * at startup (after a reboot the remote map is empty; after a restart the
 * announcements that came while the daemon was down are missing) and when its
 * sync socket had to be reopened. A requester asks at most once per
 * interval/2; a request that gets lost (a single datagram) costs the wait for
 * the peers' next regular round. A peer honours a request from the same peer
 * at most once per interval/4, and pulls rounds forward for requests at most
 * twice per interval, whoever asked (a budget that refills): a round started
 * meanwhile serves them all, and forged requests from many peer addresses
 * cannot make it run rounds back to back.
 *
 * A peer that comes back: every peer sends at least a heartbeat per interval,
 * so a datagram from a peer whose last one is more than one and a half
 * intervals old means the path was down or lost at least that heartbeat, and
 * with it whatever announcements the peer made meanwhile. That peer is asked
 * for a round at once (one request per peer per interval/2), instead of
 * waiting up to an interval for its next regular one. Both sides of a
 * partition see the other come back, so both ask.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <string.h>
#include <syslog.h>

#include "flowsync.h"

static bool request_pending;		/* ask the peers for a round */
static uint64_t last_request;		/* when we last asked (mono ms) */
static bool round_wanted;		/* a peer asked us for a round */
static uint64_t peer_asked[MAX_PEERS];	/* last request honoured, per peer */
static bool peer_silent[MAX_PEERS];
static uint32_t started;		/* now_s() at startup */
static double budget = 2;		/* rounds we may pull forward for requests */
static uint64_t budget_ts;

/* ask the peers for a round as soon as allowed */
void resync_request(void)
{
	request_pending = true;
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

/* rx: a datagram from a peer silent for longer than a heartbeat spacing
 * (silence: seconds since its last one). Ask it for a round, paced per peer. */
void resync_peer_back(int peer, uint32_t silence)
{
	static uint64_t asked_back[MAX_PEERS];
	char abuf[INET6_ADDRSTRLEN];
	uint64_t now = mono_ms();

	if (asked_back[peer] && now - asked_back[peer] < cfg.interval * 1000 / 2)
		return;
	asked_back[peer] = now;
	if (!dgram_control_to(peer, WIRE_F_RESYNC))
		return;
	cnt.tx_resync++;
	logmsg(LOG_NOTICE, "peer %s: back after %u s without a datagram (a heartbeat was lost): "
	       "asked it for a round", addr_str(&cfg.peer[peer], abuf, sizeof(abuf)), silence);
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

/* a round started: it serves every request that came before it */
void resync_round_started(void)
{
	round_wanted = false;
}

/* main loop: send a pending request if the rate allows */
void resync_tick(void)
{
	uint64_t now = mono_ms();

	if (!request_pending || (last_request && now - last_request < cfg.interval * 1000 / 2))
		return;
	/* no socket while the interface is away: it goes out once there is one */
	if (udp_fd < 0)
		return;
	last_request = now;
	/* every send failed: still pending, tried again after the spacing */
	if (!dgram_control(WIRE_F_RESYNC))
		return;
	request_pending = false;
	cnt.tx_resync++;
}

void resync_init(void)
{
	started = now_s();
	resync_request();
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
