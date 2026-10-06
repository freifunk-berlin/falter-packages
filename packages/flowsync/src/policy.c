// SPDX-License-Identifier: GPL-2.0-only
/* which flows are synced: applied identically on TX and RX */

#include <netinet/in.h>
#include <string.h>

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

/* an address a flow to the outside can have: not unspecified, loopback,
 * multicast, link-local or v4-mapped */
static bool routable(const struct in6_addr *a)
{
	return !IN6_IS_ADDR_UNSPECIFIED(a) && !IN6_IS_ADDR_LOOPBACK(a) &&
	       !IN6_IS_ADDR_MULTICAST(a) && !IN6_IS_ADDR_LINKLOCAL(a) &&
	       !IN6_IS_ADDR_V4MAPPED(a);
}

/*
 * The local map holds every flow the gateway forwarded out of its uplink,
 * whatever its addresses and protocol: that is what replaces conntrack for
 * its own replies. Only the flows that pass here are announced to the peers
 * and taken from them.
 */
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
