// SPDX-License-Identifier: GPL-2.0-only
/* the filter policy, applied identically on TX and RX */

#include <netinet/in.h>
#include <string.h>
#include <linux/netfilter/nf_conntrack_common.h>

#include "flowsync.h"

const char *proto_name(uint8_t proto)
{
	switch (proto) {
	case IPPROTO_UDP:
		return "udp";
	case IPPROTO_TCP:
		return "tcp";
	}
	return NULL;
}

int proto_num(const char *s)
{
	if (!strcmp(s, "udp"))
		return IPPROTO_UDP;
	if (!strcmp(s, "tcp"))
		return IPPROTO_TCP;
	return -1;
}

bool skip_port(unsigned int port)
{
	return cfg.skip_port[port / 8] & (1 << (port % 8));
}

/* an entry created by this daemon (or a previous instance of it) */
bool is_copy(uint32_t mark)
{
	return cfg.ct_mark && (mark & cfg.ct_mark_mask) == cfg.ct_mark;
}

/*
 * A copy is announced only with evidence of a packet since the last time we
 * looked at it. Without packets the kernel lets the timeout decay, one second
 * per second; only a packet (or our own create or refresh) sets it to
 * something else. So the copy shows traffic when its remaining timeout
 * differs from what plain decay since the baseline would leave. Mostly a
 * packet raises it (to the protocol's natural timeout: UDP stream, TCP
 * unacknowledged 300 s); it can also lower it, e.g. a copy holding the TCP
 * established timeout (hours) is capped to unacknowledged by a segment the
 * other side never acknowledges. Copies without traffic stay silent, and no
 * chain of announcements can sustain itself: every announcement traces back
 * to a packet within one round.
 *
 * The baseline (prev_remaining at prev_at) is the later of what the dump saw
 * and what we set ourselves (a create or refresh sets exactly
 * element_timeout). Without a baseline (a restart, an evicted slot) there is
 * no evidence: the copy is only remembered this round and can show traffic
 * from the next one on. Otherwise a long TCP timeout would be read as a
 * packet that may be hours old.
 */
bool copy_live(uint32_t status, uint32_t remaining, uint32_t prev_remaining, uint32_t prev_at,
	       uint32_t now)
{
	uint32_t decayed;

	if (!(status & IPS_ASSURED) || !prev_at)
		return false;
	decayed = prev_remaining > now - prev_at ? prev_remaining - (now - prev_at) : 0;
	/* two seconds of slack for rounding, in either direction */
	return remaining > decayed + 2 || remaining + 2 < decayed;
}

/* an address a flow to the outside can have: not unspecified, loopback,
 * multicast, link-local or v4-mapped */
static bool routable(const struct in6_addr *a)
{
	return !IN6_IS_ADDR_UNSPECIFIED(a) && !IN6_IS_ADDR_LOOPBACK(a) &&
	       !IN6_IS_ADDR_MULTICAST(a) && !IN6_IS_ADDR_LINKLOCAL(a) &&
	       !IN6_IS_ADDR_V4MAPPED(a);
}

/* the filter policy, applied identically on TX and RX */
bool wanted(const struct flow *f)
{
	if (!cfg.proto[f->proto])
		return false;
	if (!f->cport || !f->sport)
		return false;
	if (!routable(&f->c) || !routable(&f->s))
		return false;
	if (!in_list(&f->c, &cfg.prefix) || in_list(&f->c, &cfg.exclude))
		return false;
	if (in_list(&f->s, &cfg.exclude_dst))
		return false;
	/* the skip list is for UDP services handled statelessly (DNS); TCP to
	 * the same port (DNS over TCP) has no such path and is synced */
	if (f->proto == IPPROTO_UDP && skip_port(f->sport))
		return false;
	return true;
}
