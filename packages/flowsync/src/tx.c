// SPDX-License-Identifier: GPL-2.0-only
/*
 * Announcements.
 *
 * Events: the egress program reports the first packet of every flow; a
 * wanted one is announced to all peers at once.
 *
 * Rounds: every interval the local map is walked. A flow that has outlived
 * its timeout is deleted, every other wanted one is announced again (a peer
 * holds it for element_timeout after the last announcement). Then the remote
 * map is walked for what has expired. The walk is taken a few batches per
 * loop iteration and only while the send queue has room; the queue is sent at
 * tx_rate datagrams per second per peer.
 */

#include <errno.h>
#include <string.h>
#include <syslog.h>

#include "flowsync.h"
#include "dp.h"

static struct flow queue[Q_SIZE];
static size_t q_len, q_pos;
static double tokens;
static uint64_t tokens_ts;

static struct dp_walk *walk;
static enum dp_map walk_map;
static uint64_t round_start;
static uint64_t round_entries, round_local, round_remote;

/* new flows whose announcement did not get out: the ring was full, or the
 * send buffer of some peer; the next round carries them */
static bool ev_lost;
static uint64_t ring_lost;

void tx_event(const struct flow *f)
{
	if (!wanted(f))
		return;
	flow_dbg("tx new", f);
	dgram_add(f);
	cnt.tx_events++;
}

static void ring_check(void)
{
	struct fs_stats st;

	if (dp_stats(&st) || st.ev_lost == ring_lost)
		return;
	cnt.ev_overruns += st.ev_lost - ring_lost;
	ring_lost = st.ev_lost;
	ev_lost = true;
}

/* after a batch of events */
void tx_events_done(void)
{
	if (!dgram_flush())
		ev_lost = true;
	ring_check();
}

bool events_lost(void)
{
	return ev_lost;
}

/* queued entries, a held datagram, and a walk in progress each count */
size_t refresh_pending(void)
{
	return q_len - q_pos + (dgram_held() ? 1 : 0) + (walk ? 1 : 0);
}

void refresh_close(void)
{
	dp_walk_end(walk);
	walk = NULL;
	gauge.refresh_running = false;
}

/* 1: a round started, 0: the previous one is still being walked or sent,
 * -1: it could not start */
int refresh_start(void)
{
	static uint64_t last_log;

	if (refresh_pending()) {
		cnt.refresh_overrun++;
		if (log_ok(&last_log))
			logmsg(LOG_WARNING, "refresh: previous round not finished after %lu s "
			       "(%zu entries still to send), next one delayed (raise tx_rate)",
			       cfg.interval, q_len - q_pos);
		return 0;
	}
	walk = dp_walk_start(DP_LOCAL);
	if (!walk) {
		cnt.refresh_errors++;
		return -1;
	}
	walk_map = DP_LOCAL;
	round_start = mono_ms();
	round_entries = round_local = round_remote = 0;
	gauge.refresh_running = true;
	/* the walk finds the flows whose events were lost before it */
	ring_check();
	ev_lost = false;
	return 1;
}

static void round_done(void)
{
	refresh_close();
	gauge.refresh_ms = mono_ms() - round_start;
	gauge.refresh_entries = round_entries;
	gauge.local = round_local;
	gauge.remote = round_remote;
	cnt.refresh_rounds++;
	DBG("refresh: round done, %llu of %llu local flows announced, %llu remote, %llu ms",
	    (unsigned long long)round_entries, (unsigned long long)round_local,
	    (unsigned long long)round_remote, (unsigned long long)gauge.refresh_ms);
}

/* one batch of the walk */
static void walk_step(void)
{
	static struct dp_ent ent[WALK_BATCH];
	static uint64_t last_log;
	bool done;
	int n, i;

	n = dp_walk_next(walk, ent, WALK_BATCH, &done);
	if (n < 0) {
		cnt.refresh_errors++;
		if (log_ok(&last_log))
			logmsg(LOG_WARNING, "refresh: map walk: %s, round aborted", strerror(errno));
		refresh_close();
		return;
	}
	for (i = 0; i < n; i++) {
		if (!ent[i].left) {
			/* a packet since the walk read it makes the flow anew */
			dp_delete(walk_map, &ent[i].f);
			if (walk_map == DP_LOCAL)
				cnt.local_expired++;
			else
				cnt.remote_expired++;
		} else if (walk_map == DP_REMOTE) {
			round_remote++;
		} else {
			round_local++;
			if (wanted(&ent[i].f)) {
				queue[q_len++] = ent[i].f;
				round_entries++;
			}
		}
	}
	if (!done)
		return;
	if (walk_map == DP_LOCAL) {
		dp_walk_end(walk);
		walk = dp_walk_start(DP_REMOTE);
		walk_map = DP_REMOTE;
		if (walk)
			return;
		cnt.refresh_errors++;
	}
	round_done();
}

/*
 * A round that answers a peer's resync request is sent at resync_rate: the
 * peer has lost its flows (a reboot) and every second it waits is a second of
 * rejected replies. The send buffer's back-pressure (held datagrams) still
 * bounds it.
 */
static bool fast_round;

void refresh_fast(bool fast)
{
	fast_round = fast;
}

static unsigned long rate(void)
{
	return fast_round && cfg.resync_rate ? cfg.resync_rate : cfg.tx_rate;
}

static double burst_size(void)
{
	double burst = rate() / 20.0;

	return burst < 1 ? 1 : burst;
}

/* poll timeout while a round is pending: one burst worth of time */
int refresh_pace_ms(void)
{
	int ms = (int)(1000.0 * burst_size() / rate());

	return ms < 10 ? 10 : ms;
}

/* walk on while the queue has room, and send queued entries at tx_rate
 * (resync_rate) datagrams per second (per peer) */
void refresh_tick(void)
{
	uint64_t now = mono_ms();
	double burst = burst_size();
	unsigned int i, steps;

	tokens += (now - tokens_ts) * (double)rate() / 1000.0;
	tokens_ts = now;
	if (tokens > burst)
		tokens = burst;

	if (q_pos == q_len) {
		q_pos = q_len = 0;
	} else if (q_pos) {
		memmove(queue, queue + q_pos, (q_len - q_pos) * sizeof(*queue));
		q_len -= q_pos;
		q_pos = 0;
	}
	for (steps = 0; walk && steps < WALK_STEPS && Q_SIZE - q_len >= WALK_BATCH; steps++)
		walk_step();

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
