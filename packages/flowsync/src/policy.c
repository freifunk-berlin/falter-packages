// SPDX-License-Identifier: GPL-2.0-only
/* which flows are synced: applied identically on TX and RX */

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#include "flowsync.h"

/* the protocols with a name here; any other goes by its number */
static const struct {
	const char *name;
	uint8_t num;
} protos[] = {
	{ "tcp", 6 },
	{ "udp", 17 },
	{ "sctp", 132 },
	{ "esp", 50 },		/* IPsec without UDP encapsulation */
	{ "gre", 47 },
	{ "ipip", 4 },		/* IPv4 in IPv6 */
	{ "ip6ip6", 41 },	/* IPv6 in IPv6 */
	{ "l2tp", 115 },	/* L2TPv3 over IP */
};
#define N_PROTOS (sizeof(protos) / sizeof(protos[0]))

const char *proto_name(uint8_t proto)
{
	unsigned int i;

	for (i = 0; i < N_PROTOS; i++)
		if (protos[i].num == proto)
			return protos[i].name;
	return NULL;
}

/* the name, or the number for a protocol without one */
const char *proto_str(uint8_t proto, char *buf, size_t len)
{
	if (proto_name(proto))
		return proto_name(proto);
	snprintf(buf, len, "%u", proto);
	return buf;
}

/* a name from the table or a number 1..255; -1 for anything else */
int proto_num(const char *s)
{
	unsigned int i;
	char *end;
	long v;

	for (i = 0; i < N_PROTOS; i++)
		if (!strcmp(s, protos[i].name))
			return protos[i].num;
	v = strtol(s, &end, 10);
	if (end == s || *end || v < 1 || v > 255)
		return -1;
	return v;
}

/* the protocols whose flows have two ports (as the tc programs key them) */
bool proto_ports(uint8_t proto)
{
	return proto == IPPROTO_TCP || proto == IPPROTO_UDP || proto == IPPROTO_SCTP;
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
	/* the shape the tc programs give a flow of this protocol: two ports,
	 * or nothing but the addresses. ICMPv6 has no flows there. */
	if (f->proto == IPPROTO_ICMPV6)
		return false;
	if (proto_ports(f->proto)) {
		if (!f->cport || !f->sport)
			return false;
	} else if (f->cport || f->sport) {
		return false;
	}
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
