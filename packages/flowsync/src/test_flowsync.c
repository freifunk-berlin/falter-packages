// SPDX-License-Identifier: GPL-2.0-only
/*
 * Unit test for everything that does not need a kernel: prefixes, policy, the
 * wire format, the options and the lifetimes of flows. Build and run with
 * "make test" (TEST_RUNNER=qemu-... when cross compiling). The kernel-side
 * behaviour is covered by the data path tests in test/gwlab.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <endian.h>

#include "flowsync.h"
#include "dp.h"
#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netlink.h>

/* config.c sets it; status.c, which defines it, needs the datapath */
const char *status_path;

static unsigned int checks, failures;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static struct in6_addr a6(const char *s)
{
	struct in6_addr a;

	if (inet_pton(AF_INET6, s, &a) != 1) {
		fprintf(stderr, "bad test address %s\n", s);
		exit(2);
	}
	return a;
}

static bool addr_eq(const struct in6_addr *a, const char *s)
{
	struct in6_addr b = a6(s);

	return IN6_ARE_ADDR_EQUAL(a, &b);
}

static bool listed(const char *s, const struct prefix_list *l)
{
	struct in6_addr a = a6(s);

	return in_list(&a, l);
}

static struct flow mkflow(uint8_t proto, uint16_t cport, uint16_t sport)
{
	struct flow f;

	memset(&f, 0, sizeof(f));
	f.c = a6("2001:db8:100::1");
	f.s = a6("2a00:1450:4001:81a::200e");
	f.cport = cport;
	f.sport = sport;
	f.proto = proto;
	return f;
}

static bool flow_eq(const struct flow *a, const struct flow *b)
{
	return a->proto == b->proto && a->cport == b->cport && a->sport == b->sport &&
	       IN6_ARE_ADDR_EQUAL(&a->c, &b->c) && IN6_ARE_ADDR_EQUAL(&a->s, &b->s);
}

static void test_prefix(void)
{
	struct prefix p;
	struct prefix_list l = { .n = 0 };

	CHECK(parse_prefix("2001:db8:1ff::/44", &p) == 0);
	CHECK(p.len == 44);
	CHECK(addr_eq(&p.addr, "2001:db8:1f0::"));
	CHECK(parse_prefix("2001:db8::1", &p) == 0 && p.len == 128);
	CHECK(parse_prefix("2001:db8::/129", &p) != 0);
	CHECK(parse_prefix("2001:db8::/x", &p) != 0);
	CHECK(parse_prefix("not-an-address/64", &p) != 0);
	CHECK(parse_prefix("::/0", &p) == 0 && p.len == 0);

	parse_prefix("2001:db8:1ff::/44", &l.p[l.n++]);		/* bit boundary */
	parse_prefix("2001:db8:ffff::/48", &l.p[l.n++]);	/* byte boundary */
	parse_prefix("2001:db8:2::1/128", &l.p[l.n++]);		/* host */
	CHECK(listed("2001:db8:1f5::1", &l));
	CHECK(listed("2001:db8:1ff:ffff::1", &l));
	CHECK(!listed("2001:db8:200::1", &l));
	CHECK(!listed("2001:db8:1e0::1", &l));
	CHECK(listed("2001:db8:ffff:1::", &l));
	CHECK(!listed("2001:db8:fffe::", &l));
	CHECK(listed("2001:db8:2::1", &l));
	CHECK(!listed("2001:db8:2::2", &l));

	l.n = 0;
	parse_prefix("::/0", &l.p[l.n++]);
	CHECK(listed("fe80::1", &l));
}

static void test_addr_port(void)
{
	struct in6_addr a;
	char buf[INET6_ADDRSTRLEN];
	uint16_t port;

	CHECK(parse_addr("192.0.2.1", &a) == 0);
	CHECK(IN6_IS_ADDR_V4MAPPED(&a));
	CHECK(!strcmp(addr_str(&a, buf, sizeof(buf)), "192.0.2.1"));
	CHECK(parse_addr("2001:db8::1", &a) == 0);
	CHECK(!strcmp(addr_str(&a, buf, sizeof(buf)), "2001:db8::1"));
	CHECK(parse_addr("nope", &a) != 0);

	CHECK(parse_port("0", &port) == 0 && port == 0);
	CHECK(parse_port("65535", &port) == 0 && port == 65535);
	CHECK(parse_port("65536", &port) != 0);
	CHECK(parse_port("-1", &port) != 0);
	CHECK(parse_port("12a", &port) != 0);
	CHECK(parse_port("", &port) != 0);
}

static void setup_cfg(void)
{
	memset(&cfg, 0, sizeof(cfg));
	cfg.interval = 30;
	cfg.element_timeout = 90;
	cfg.batch_lines = MAX_BATCH;
	cfg.mark = 0x01000000;
	cfg.proto[IPPROTO_UDP] = true;
	cfg.proto[IPPROTO_TCP] = true;
	cfg.n_proto = 2;
	cfg.skip_port[53 / 8] |= 1 << (53 % 8);
	parse_prefix("2001:db8:100::/44", &cfg.prefix.p[cfg.prefix.n++]);
	parse_prefix("2001:db8:10f::/48", &cfg.exclude.p[cfg.exclude.n++]);
	parse_prefix("2001:db8::/32", &cfg.exclude_dst.p[cfg.exclude_dst.n++]);
}

static void test_policy(void)
{
	struct flow f = mkflow(IPPROTO_UDP, 50000, 443), g;

	CHECK(proto_num("udp") == IPPROTO_UDP);
	CHECK(proto_num("tcp") == IPPROTO_TCP);
	CHECK(proto_num("sctp") == IPPROTO_SCTP && proto_num("esp") == IPPROTO_ESP);
	CHECK(proto_num("gre") == 47 && proto_num("47") == 47 && proto_num("l2tp") == 115);
	CHECK(proto_num("ipip") == 4 && proto_num("ip6ip6") == 41);
	CHECK(proto_num("0") < 0 && proto_num("256") < 0 && proto_num("udpx") < 0 && proto_num("") < 0);
	CHECK(!strcmp(proto_name(IPPROTO_UDP), "udp"));
	CHECK(proto_name(1) == NULL);
	CHECK(proto_ports(IPPROTO_TCP) && proto_ports(IPPROTO_SCTP) && !proto_ports(IPPROTO_ESP));
	CHECK(skip_port(53) && !skip_port(54));

	CHECK(wanted(&f));
	g = f; g.sport = 53;
	CHECK(!wanted(&g));
	g.proto = IPPROTO_TCP;				/* DNS over TCP is synced */
	CHECK(wanted(&g));
	g = f; g.proto = IPPROTO_TCP;			/* tcp is configured */
	CHECK(wanted(&g));
	g = f; g.proto = IPPROTO_ICMPV6;		/* not configured */
	CHECK(!wanted(&g));
	/* the shape of a flow goes with its protocol */
	cfg.proto[IPPROTO_ESP] = cfg.proto[IPPROTO_ICMPV6] = cfg.proto[IPPROTO_SCTP] = true;
	g = f; g.proto = IPPROTO_ESP;			/* no ports */
	CHECK(!wanted(&g));
	g.cport = g.sport = 0;
	CHECK(wanted(&g));
	g.proto = IPPROTO_ICMPV6;			/* never a flow, whatever is configured */
	CHECK(!wanted(&g));
	g = f; g.proto = IPPROTO_SCTP;			/* two ports */
	CHECK(wanted(&g));
	g.sport = 0;
	CHECK(!wanted(&g));
	cfg.proto[IPPROTO_ESP] = cfg.proto[IPPROTO_ICMPV6] = cfg.proto[IPPROTO_SCTP] = false;
	g = f; g.cport = 0;
	CHECK(!wanted(&g));
	g = f; g.sport = 0;
	CHECK(!wanted(&g));
	g = f; g.c = a6("2001:db8:10f::1");			/* excluded client */
	CHECK(!wanted(&g));
	g = f; g.c = a6("2001:db8:200::1");			/* outside prefix */
	CHECK(!wanted(&g));
	g = f; g.s = a6("2001:db8:7::1");			/* mesh-internal server */
	CHECK(!wanted(&g));
	g = f; g.c = a6("2001:db8:10e:ffff::1");		/* inside prefix, not excluded */
	CHECK(wanted(&g));
	g = f; g.s = a6("fe80::1");				/* no flow to the outside */
	CHECK(!wanted(&g));
	g = f; g.s = a6("ff02::1");
	CHECK(!wanted(&g));
	g = f; g.s = a6("::1");
	CHECK(!wanted(&g));
	g = f; g.s = a6("::ffff:192.0.2.1");
	CHECK(!wanted(&g));
	g = f; g.s = a6("::");
	CHECK(!wanted(&g));
}

static void test_wire(void)
{
	struct flow f = mkflow(IPPROTO_UDP, 50000, 443), g;
	uint8_t buf[MAX_DGRAM], fl = 0;
	unsigned int count = 0, i;

	CHECK(WIRE_MAX_RECORDS == 34);
	CHECK(WIRE_HDR_LEN + WIRE_MAX_RECORDS * WIRE_REC_LEN <= MAX_DGRAM);
	/* the default datagram fits the IPv6 minimum MTU with IPv6 and UDP headers */
	CHECK(40 + 8 + WIRE_HDR_LEN + MAX_BATCH * WIRE_REC_LEN <= 1280);

	wire_put_hdr(buf, 2, 0);
	wire_put(buf + WIRE_HDR_LEN, &f);
	f.cport = 1; f.sport = 65535;
	wire_put(buf + WIRE_HDR_LEN + WIRE_REC_LEN, &f);
	CHECK(buf[0] == WIRE_VERSION && buf[1] == 2 && buf[2] == 0 && buf[3] == 0);
	/* ports in network byte order at fixed offsets */
	CHECK(buf[WIRE_HDR_LEN + 2] == (50000 >> 8) && buf[WIRE_HDR_LEN + 3] == (50000 & 0xff));
	CHECK(buf[WIRE_HDR_LEN + 4] == (443 >> 8) && buf[WIRE_HDR_LEN + 5] == (443 & 0xff));
	CHECK(buf[WIRE_HDR_LEN + 8] == 0x20 && buf[WIRE_HDR_LEN + 9] == 0x01);

	CHECK(wire_check(buf, WIRE_HDR_LEN + 2 * WIRE_REC_LEN, &count, &fl) == PARSE_OK && count == 2);
	CHECK(wire_get(buf + WIRE_HDR_LEN + WIRE_REC_LEN, &g) == PARSE_OK);
	CHECK(flow_eq(&f, &g));
	f.cport = 50000; f.sport = 443;
	CHECK(wire_get(buf + WIRE_HDR_LEN, &g) == PARSE_OK);
	CHECK(flow_eq(&f, &g));

	/* rejections */
	CHECK(wire_check(buf, 3, &count, &fl) == PARSE_ERR);
	CHECK(wire_check(buf, WIRE_HDR_LEN + 2 * WIRE_REC_LEN - 1, &count, &fl) == PARSE_ERR);
	CHECK(wire_check(buf, WIRE_HDR_LEN + 2 * WIRE_REC_LEN + 1, &count, &fl) == PARSE_ERR);
	CHECK(wire_check(buf, WIRE_HDR_LEN + 1 * WIRE_REC_LEN, &count, &fl) == PARSE_ERR);
	buf[1] = 0;					/* heartbeat: header only */
	CHECK(wire_check(buf, WIRE_HDR_LEN, &count, &fl) == PARSE_OK && count == 0 && fl == 0);
	CHECK(wire_check(buf, WIRE_HDR_LEN + 1, &count, &fl) == PARSE_ERR);
	wire_put_hdr(buf, 0, WIRE_F_RESYNC);		/* resync request */
	CHECK(wire_check(buf, WIRE_HDR_LEN, &count, &fl) == PARSE_OK && (fl & WIRE_F_RESYNC));
	wire_put_hdr(buf, 2, 0);
	buf[1] = WIRE_MAX_RECORDS + 1;
	CHECK(wire_check(buf, WIRE_HDR_LEN + (WIRE_MAX_RECORDS + 1) * WIRE_REC_LEN, &count, &fl) == PARSE_ERR);
	buf[1] = 2;
	buf[0] = 2;
	CHECK(wire_check(buf, WIRE_HDR_LEN + 2 * WIRE_REC_LEN, &count, &fl) == PARSE_VERSION);
	buf[0] = '1';	/* the old text format */
	CHECK(wire_check(buf, WIRE_HDR_LEN + 2 * WIRE_REC_LEN, &count, &fl) == PARSE_VERSION);
	buf[0] = WIRE_VERSION;
	buf[WIRE_HDR_LEN] = 0;	/* no protocol; every other number is one, the policy decides */
	CHECK(wire_get(buf + WIRE_HDR_LEN, &g) == PARSE_ERR);
	buf[WIRE_HDR_LEN] = 50;
	CHECK(wire_get(buf + WIRE_HDR_LEN, &g) == PARSE_OK && g.proto == 50);
	buf[WIRE_HDR_LEN] = 17;
	CHECK(wire_get(buf + WIRE_HDR_LEN, &g) == PARSE_OK);
	buf[WIRE_HDR_LEN + 1] = 1;	/* record flags: zero in version 1 */
	CHECK(wire_get(buf + WIRE_HDR_LEN, &g) == PARSE_ERR);
	buf[WIRE_HDR_LEN + 1] = 0;
	buf[WIRE_HDR_LEN + 7] = 1;	/* reserved */
	CHECK(wire_get(buf + WIRE_HDR_LEN, &g) == PARSE_ERR);
	buf[WIRE_HDR_LEN + 7] = 0;
	buf[3] = 1;			/* header reserved byte */
	CHECK(wire_check(buf, WIRE_HDR_LEN + 2 * WIRE_REC_LEN, &count, &fl) == PARSE_ERR);
	buf[3] = 0;

	/* a full datagram: 34 records are still accepted (older senders, batch_lines 34) */
	wire_put_hdr(buf, WIRE_MAX_RECORDS, 0);
	for (i = 0; i < WIRE_MAX_RECORDS; i++)
		wire_put(buf + WIRE_HDR_LEN + i * WIRE_REC_LEN, &f);
	CHECK(wire_check(buf, WIRE_HDR_LEN + WIRE_MAX_RECORDS * WIRE_REC_LEN, &count, &fl) == PARSE_OK &&
	      count == WIRE_MAX_RECORDS);

	/* a TCP record round trips the same way */
	f.proto = IPPROTO_TCP; f.cport = 51000; f.sport = 80;
	wire_put(buf + WIRE_HDR_LEN, &f);
	CHECK(wire_get(buf + WIRE_HDR_LEN, &g) == PARSE_OK);
	CHECK(flow_eq(&f, &g) && g.proto == IPPROTO_TCP);
}

/* the lifetimes the tc programs and the daemon both compute */
static void test_ttl(void)
{
	struct fs_cfg c = { .t_udp = 180, .t_tcp_syn = 120, .t_tcp_est = 7440, .t_tcp_close = 60,
			    .t_other = 600 };

	CHECK(fs_ttl(&c, IPPROTO_UDP, 0) == 180);
	CHECK(fs_ttl(&c, IPPROTO_UDP, FS_F_EST | FS_F_CLOSING) == 180);
	CHECK(fs_ttl(&c, IPPROTO_TCP, 0) == 120);
	CHECK(fs_ttl(&c, IPPROTO_TCP, FS_F_EST) == 7440);
	CHECK(fs_ttl(&c, IPPROTO_TCP, FS_F_EST | FS_F_CLOSING) == 60);
	CHECK(fs_ttl(&c, IPPROTO_TCP, FS_F_CLOSING) == 60);
	CHECK(fs_ttl(&c, 47, 0) == 600);
	/* the map key has no padding the compiler chose */
	CHECK(sizeof(struct fs_key) == 40);
	CHECK(sizeof(struct fs_local) == 8);
}

/* numbers on the command line: decimal, the mark hexadecimal with or without
 * 0x (written as nft prints it); no octal */
static void test_config(void)
{
	char *a1[] = { "flowsync", "-x", "2001:db8::/32", "-e", "192.0.2.1", "-m", "01000000",
		       "-I", "eth0", "-i", "010", "-p", "03780", "check", NULL };
	char *a2[] = { "flowsync", "-x", "2001:db8::/32", "-e", "192.0.2.1", "-m", "0x2000000",
		       "-U", "wan", "-I", "eth0", "--tcp-timeout", "300", "check", NULL };
	char *a3[] = { "flowsync", "-x", "2001:db8::/32", "-e", "192.0.2.1", "-i", "0x10",
		       "check", NULL };
	/* IPv4 peers and bind address: v4-mapped, of one family */
	char *a4[] = { "flowsync", "-b", "10.0.0.1", "-e", "10.0.0.2", "-e", "10.0.0.3", "check",
		       NULL };
	char *a5[] = { "flowsync", "-b", "10.0.0.1", "-e", "fd00::2", "check", NULL };

	optind = 1;
	CHECK(parse_args(14, a1) == 13);
	CHECK(cfg.mark == 0x01000000);
	CHECK(cfg.interval == 10 && cfg.port == 3780);
	CHECK(!strcmp(cfg.uplink, "eth0"));		/* the sync device by default */
	CHECK(cfg.t_udp == 180 && cfg.t_tcp == 7440);
	optind = 1;
	CHECK(parse_args(14, a2) == 13);
	CHECK(cfg.mark == 0x02000000 && cfg.t_tcp == 300);
	CHECK(!strcmp(cfg.uplink, "wan") && !strcmp(cfg.ifname, "eth0"));
	optind = 1;
	CHECK(parse_args(8, a3) < 0);				/* not a decimal number */
	optind = 1;
	CHECK(parse_args(8, a4) == 7);
	CHECK(cfg.bind_set && IN6_IS_ADDR_V4MAPPED(&cfg.bind) && cfg.n_peer == 2 &&
	      IN6_IS_ADDR_V4MAPPED(&cfg.peer[0]) && IN6_IS_ADDR_V4MAPPED(&cfg.peer[1]));
	CHECK(cfg.port == 3994);				/* the default, not conntrackd's 3780 */
	optind = 1;
	CHECK(parse_args(6, a5) < 0);				/* different address families */
	setup_cfg();
}

/* the alive element batch, as the kernel will parse it: three messages, the
 * middle one for ip6 flowsync/alive with key, timeout and expiration */
static const struct nlattr *attr_find(const struct nlattr *a, size_t len, uint16_t type)
{
	while (len >= NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= len) {
		if ((a->nla_type & NLA_TYPE_MASK) == type)
			return a;
		len -= NLA_ALIGN(a->nla_len);
		a = (const struct nlattr *)((const char *)a + NLA_ALIGN(a->nla_len));
	}
	return NULL;
}

static void test_nfnl(void)
{
	char buf[512];
	const struct nlmsghdr *h;
	const struct nfgenmsg *g;
	const struct nlattr *a, *elems, *elem, *key, *val, *to, *ex;
	size_t len, n = 0;
	uint64_t be;
	uint32_t idx;

	len = nfnl_alive_msg(buf, sizeof(buf), true, 7, 10, 100, 4242);
	CHECK(len > 0);
	for (h = (const struct nlmsghdr *)buf; NLMSG_OK(h, len); h = NLMSG_NEXT(h, len), n++) {
		CHECK(h->nlmsg_pid == 4242 && h->nlmsg_seq == 100 + n);
		if (n != 1) {
			CHECK(h->nlmsg_type == (n ? NFNL_MSG_BATCH_END : NFNL_MSG_BATCH_BEGIN));
			continue;
		}
		CHECK(h->nlmsg_type == ((NFNL_SUBSYS_NFTABLES << 8) | NFT_MSG_NEWSETELEM));
		CHECK(h->nlmsg_flags == (NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE));
		g = NLMSG_DATA(h);
		CHECK(g->nfgen_family == NFPROTO_IPV6);
		a = (const struct nlattr *)((const char *)g + NLMSG_ALIGN(sizeof(*g)));
		len = 0;		/* reuse: attribute bytes */
		len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*g));
		CHECK(attr_find(a, len, NFTA_SET_ELEM_LIST_TABLE) &&
		      !strcmp((const char *)attr_find(a, len, NFTA_SET_ELEM_LIST_TABLE) + NLA_HDRLEN,
			      "flowsync"));
		CHECK(attr_find(a, len, NFTA_SET_ELEM_LIST_SET) &&
		      !strcmp((const char *)attr_find(a, len, NFTA_SET_ELEM_LIST_SET) + NLA_HDRLEN,
			      "alive"));
		elems = attr_find(a, len, NFTA_SET_ELEM_LIST_ELEMENTS);
		CHECK(elems && (elems->nla_type & NLA_F_NESTED));
		elem = attr_find((const struct nlattr *)((const char *)elems + NLA_HDRLEN),
				 elems->nla_len - NLA_HDRLEN, NFTA_LIST_ELEM);
		CHECK(elem != NULL);
		key = attr_find((const struct nlattr *)((const char *)elem + NLA_HDRLEN),
				elem->nla_len - NLA_HDRLEN, NFTA_SET_ELEM_KEY);
		CHECK(key != NULL);
		val = attr_find((const struct nlattr *)((const char *)key + NLA_HDRLEN),
				key->nla_len - NLA_HDRLEN, NFTA_DATA_VALUE);
		CHECK(val && val->nla_len == NLA_HDRLEN + 4);
		memcpy(&idx, (const char *)val + NLA_HDRLEN, 4);
		CHECK(idx == 7);
		to = attr_find((const struct nlattr *)((const char *)elem + NLA_HDRLEN),
			       elem->nla_len - NLA_HDRLEN, NFTA_SET_ELEM_TIMEOUT);
		ex = attr_find((const struct nlattr *)((const char *)elem + NLA_HDRLEN),
			       elem->nla_len - NLA_HDRLEN, NFTA_SET_ELEM_EXPIRATION);
		CHECK(to && ex && to->nla_len == NLA_HDRLEN + 8);
		memcpy(&be, (const char *)to + NLA_HDRLEN, 8);
		CHECK(be64toh(be) == 10000);
		memcpy(&be, (const char *)ex + NLA_HDRLEN, 8);
		CHECK(be64toh(be) == 10000);
		len = (const char *)buf + nfnl_alive_msg(buf, sizeof(buf), true, 7, 10, 100, 4242) -
		      (const char *)h;	/* the walk goes on from here */
	}
	CHECK(n == 3);
	/* a delete carries the key only */
	len = nfnl_alive_msg(buf, sizeof(buf), false, 7, 10, 200, 4242);
	h = (const struct nlmsghdr *)(buf + NLMSG_SPACE(sizeof(struct nfgenmsg)));
	CHECK(h->nlmsg_type == ((NFNL_SUBSYS_NFTABLES << 8) | NFT_MSG_DELSETELEM));
	CHECK(h->nlmsg_flags == (NLM_F_REQUEST | NLM_F_ACK));
	g = NLMSG_DATA(h);
	a = (const struct nlattr *)((const char *)g + NLMSG_ALIGN(sizeof(*g)));
	elems = attr_find(a, h->nlmsg_len - NLMSG_LENGTH(sizeof(*g)), NFTA_SET_ELEM_LIST_ELEMENTS);
	elem = attr_find((const struct nlattr *)((const char *)elems + NLA_HDRLEN),
			 elems->nla_len - NLA_HDRLEN, NFTA_LIST_ELEM);
	CHECK(elem && !attr_find((const struct nlattr *)((const char *)elem + NLA_HDRLEN),
				 elem->nla_len - NLA_HDRLEN, NFTA_SET_ELEM_TIMEOUT));
	CHECK(!nfnl_alive_msg(buf, 64, true, 7, 10, 1, 1));	/* does not fit: 0 */
}

int main(void)
{
	setup_cfg();
	test_prefix();
	test_addr_port();
	test_policy();
	test_wire();
	test_ttl();
	test_config();
	test_nfnl();
	printf("%s: %u checks, %u failures\n", failures ? "FAIL" : "ok", checks, failures);
	return failures ? 1 : 0;
}
