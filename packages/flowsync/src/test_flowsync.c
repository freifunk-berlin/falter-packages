// SPDX-License-Identifier: GPL-2.0-only
/*
 * Unit test for everything that does not need a kernel: prefixes, policy, the
 * wire format, the ctnetlink message builder/parser, the RX table with its
 * ownership rules and the injection bookkeeping. Build and run with
 * "make test" (TEST_RUNNER=qemu-... when cross compiling). The kernel-side
 * behaviour is covered by the integration tests in test/fstest.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libmnl/libmnl.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/netfilter/nf_conntrack_common.h>
#include <linux/netfilter/nf_conntrack_tcp.h>

#include "flowsync.h"

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

static bool flow_eq(const struct flow *a, const struct flow *b)
{
	return a->proto == b->proto && a->cport == b->cport && a->sport == b->sport &&
	       IN6_ARE_ADDR_EQUAL(&a->c, &b->c) && IN6_ARE_ADDR_EQUAL(&a->s, &b->s);
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
	cfg.ct_mark = 0x01000000;
	cfg.ct_mark_mask = 0x01000000;
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
	CHECK(proto_num("sctp") < 0);
	CHECK(!strcmp(proto_name(IPPROTO_UDP), "udp"));
	CHECK(proto_name(1) == NULL);
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

	/* mark and traffic evidence */
	CHECK(!is_copy(0));
	CHECK(is_copy(0x01000000));
	CHECK(is_copy(0x01000001));			/* other bits do not matter */
	CHECK(!is_copy(0x00000001));
	/* no baseline (restart, evicted slot): no evidence, however long the timeout */
	CHECK(!copy_live(IPS_SEEN_REPLY | IPS_ASSURED, 91, 0, 0, 1000));
	CHECK(!copy_live(IPS_SEEN_REPLY | IPS_ASSURED, 7000, 0, 0, 1000));
	/* baseline from our own create/refresh 10 s ago (90 s): a packet since */
	CHECK(copy_live(IPS_SEEN_REPLY | IPS_ASSURED, 120, 90, 990, 1000));
	CHECK(!copy_live(IPS_SEEN_REPLY | IPS_ASSURED, 90, 90, 1000, 1000));	/* exactly a refresh */
	CHECK(!copy_live(IPS_SEEN_REPLY, 120, 90, 990, 1000));		/* never saw a packet */
	/* seen 30 s ago with 120 left: plain decay leaves 90, a packet resets to 120 */
	CHECK(copy_live(IPS_SEEN_REPLY | IPS_ASSURED, 120, 120, 970, 1000));
	CHECK(!copy_live(IPS_SEEN_REPLY | IPS_ASSURED, 91, 120, 970, 1000));
	/* a TCP copy with days left is alive only if the timeout stopped decaying */
	CHECK(!copy_live(IPS_SEEN_REPLY | IPS_ASSURED, 431970, 432000, 970, 1000));
	CHECK(copy_live(IPS_SEEN_REPLY | IPS_ASSURED, 432000, 432000, 970, 1000));
	/* a packet that lowers the timeout is evidence too: an established TCP
	 * copy (days) capped to unacknowledged (300 s) by a server segment */
	CHECK(copy_live(IPS_SEEN_REPLY | IPS_ASSURED, 300, 432000, 990, 1000));
	CHECK(!copy_live(IPS_SEEN_REPLY | IPS_ASSURED, 431989, 432000, 990, 1000));	/* decay, rounding */
	/* previous sighting already decayed to nothing */
	CHECK(copy_live(IPS_SEEN_REPLY | IPS_ASSURED, 100, 5, 900, 1000));
	cfg.ct_mark = 0;
	CHECK(!is_copy(0x01000000));
	cfg.ct_mark = 0x01000000;
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
	buf[WIRE_HDR_LEN] = 1;	/* ICMPv6 is not a known protocol */
	CHECK(wire_get(buf + WIRE_HDR_LEN, &g) == PARSE_ERR);

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

static int attr_cb(const struct nlattr *attr, void *data)
{
	uint64_t *seen = data;

	if (mnl_attr_get_type(attr) < 64)
		*seen |= (uint64_t)1 << mnl_attr_get_type(attr);
	return MNL_CB_OK;
}

#define SEEN(mask, attr) (((mask) >> (attr)) & 1)

static uint64_t attrs_of(const struct nlmsghdr *nlh)
{
	uint64_t seen = 0;

	mnl_attr_parse(nlh, sizeof(struct nfgenmsg), attr_cb, &seen);
	return seen;
}

/* extract the nested TCP proto-info a TCP create emits */
struct tcp_pi { int state; bool forig, freply, present; };

static int tcp_state_cb(const struct nlattr *attr, void *data)
{
	struct tcp_pi *pi = data;

	switch (mnl_attr_get_type(attr)) {
	case CTA_PROTOINFO_TCP_STATE:          pi->state = mnl_attr_get_u8(attr); break;
	case CTA_PROTOINFO_TCP_FLAGS_ORIGINAL: pi->forig = true; break;
	case CTA_PROTOINFO_TCP_FLAGS_REPLY:    pi->freply = true; break;
	}
	return MNL_CB_OK;
}

static int protoinfo_cb(const struct nlattr *attr, void *data)
{
	if (mnl_attr_get_type(attr) == CTA_PROTOINFO_TCP) {
		((struct tcp_pi *)data)->present = true;
		mnl_attr_parse_nested(attr, tcp_state_cb, data);
	}
	return MNL_CB_OK;
}

static int top_protoinfo_cb(const struct nlattr *attr, void *data)
{
	if (mnl_attr_get_type(attr) == CTA_PROTOINFO)
		mnl_attr_parse_nested(attr, protoinfo_cb, data);
	return MNL_CB_OK;
}

static void get_tcp_pi(const struct nlmsghdr *nlh, struct tcp_pi *pi)
{
	memset(pi, 0, sizeof(*pi));
	pi->state = -1;
	mnl_attr_parse(nlh, sizeof(struct nfgenmsg), top_protoinfo_cb, pi);
}

static void test_ctnl(void)
{
	char buf[8192] __attribute__((aligned(8)));
	struct flow f = mkflow(IPPROTO_UDP, 50000, 443), t = mkflow(IPPROTO_TCP, 51000, 80);
	struct ct_entry c;
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfh;
	struct tcp_pi pi;
	uint64_t seen;

	/* a create: EXCL, everything set, and it parses back */
	nlh = mnl_nlmsg_put_header(buf);
	ct_build_new(nlh, &f, true);
	nlh->nlmsg_seq = 7;
	CHECK(nlh->nlmsg_type == ((NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_NEW));
	CHECK((nlh->nlmsg_flags & (NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL)) ==
	      (NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL));
	CHECK(nlh->nlmsg_len < 256);
	CHECK(ct_parse(nlh, &c) == 0);
	CHECK(flow_eq(&f, &c.f));
	CHECK(c.status == (IPS_SEEN_REPLY | IPS_CONFIRMED));	/* CONFIRMED echoed, never ASSURED */
	CHECK(c.mark == 0x01000000);
	CHECK(c.timeout == 90);
	seen = attrs_of(nlh);
	CHECK(SEEN(seen, CTA_TUPLE_ORIG) && SEEN(seen, CTA_TUPLE_REPLY) && SEEN(seen, CTA_TIMEOUT));
	CHECK(SEEN(seen, CTA_STATUS) && SEEN(seen, CTA_MARK) && SEEN(seen, CTA_MARK_MASK));
	CHECK(!SEEN(seen, CTA_FILTER));
	CHECK(!SEEN(seen, CTA_PROTOINFO));	/* UDP carries no proto-info */

	/* not IPv6: rejected before any attribute is looked at */
	nfh = mnl_nlmsg_get_payload(nlh);
	nfh->nfgen_family = AF_INET;
	CHECK(ct_parse(nlh, &c) != 0);
	nfh->nfgen_family = AF_INET6;
	/* truncated: the tuple is cut off */
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*nfh)) + 8;
	CHECK(ct_parse(nlh, &c) != 0);

	/* a refresh: no EXCL, tuples and timeout only, so it can change nothing
	 * else; it has no status, which is why ct_parse refuses it */
	nlh = mnl_nlmsg_put_header(buf);
	ct_build_new(nlh, &t, false);
	/* a refresh must not create: the kernel would make an unmarked native */
	CHECK(!(nlh->nlmsg_flags & (NLM_F_EXCL | NLM_F_CREATE)));
	seen = attrs_of(nlh);
	CHECK(SEEN(seen, CTA_TUPLE_ORIG) && SEEN(seen, CTA_TUPLE_REPLY) && SEEN(seen, CTA_TIMEOUT));
	CHECK(!SEEN(seen, CTA_STATUS) && !SEEN(seen, CTA_MARK) && !SEEN(seen, CTA_MARK_MASK));
	CHECK(!SEEN(seen, CTA_PROTOINFO));
	CHECK(ct_parse(nlh, &c) != 0);

	/* a TCP create carries CTA_PROTOINFO, state ESTABLISHED, both flag attrs */
	nlh = mnl_nlmsg_put_header(buf);
	ct_build_new(nlh, &t, true);
	CHECK(ct_parse(nlh, &c) == 0);
	CHECK(flow_eq(&t, &c.f) && c.f.proto == IPPROTO_TCP);
	CHECK(c.status == (IPS_SEEN_REPLY | IPS_CONFIRMED));
	CHECK(SEEN(attrs_of(nlh), CTA_PROTOINFO));
	get_tcp_pi(nlh, &pi);
	CHECK(pi.present && pi.state == TCP_CONNTRACK_ESTABLISHED);
	CHECK(pi.forig && pi.freply);

	/* dump requests: status and mark filters are independent and optional */
	nlh = mnl_nlmsg_put_header(buf);
	ct_build_dump(nlh, IPPROTO_UDP, IPS_ASSURED, IPS_ASSURED, 0, cfg.ct_mark_mask);
	CHECK(nlh->nlmsg_type == ((NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_GET));
	CHECK((nlh->nlmsg_flags & NLM_F_DUMP) == NLM_F_DUMP);
	seen = attrs_of(nlh);
	CHECK(SEEN(seen, CTA_STATUS) && SEEN(seen, CTA_STATUS_MASK));
	CHECK(SEEN(seen, CTA_MARK) && SEEN(seen, CTA_MARK_MASK));
	CHECK(SEEN(seen, CTA_TUPLE_ORIG) && SEEN(seen, CTA_FILTER));
	CHECK(!SEEN(seen, CTA_TUPLE_REPLY) && !SEEN(seen, CTA_TIMEOUT));
	nlh = mnl_nlmsg_put_header(buf);
	ct_build_dump(nlh, IPPROTO_TCP, 0, 0, cfg.ct_mark, cfg.ct_mark_mask);
	seen = attrs_of(nlh);
	CHECK(!SEEN(seen, CTA_STATUS) && !SEEN(seen, CTA_STATUS_MASK));
	CHECK(SEEN(seen, CTA_MARK) && SEEN(seen, CTA_MARK_MASK));
	nlh = mnl_nlmsg_put_header(buf);
	ct_build_dump(nlh, IPPROTO_UDP, 0, 0, 0, 0);
	seen = attrs_of(nlh);
	CHECK(!SEEN(seen, CTA_STATUS) && !SEEN(seen, CTA_MARK) && SEEN(seen, CTA_FILTER));

	/* the offloaded-native phase: offloaded and replied, but not ASSURED */
	nlh = mnl_nlmsg_put_header(buf);
	ct_build_dump(nlh, IPPROTO_UDP, IPS_OFFLOAD | IPS_SEEN_REPLY,
		      IPS_OFFLOAD | IPS_SEEN_REPLY | IPS_ASSURED, 0, cfg.ct_mark_mask);
	{
		const struct nlattr *a;
		uint32_t st = 0, mask = 0;

		mnl_attr_for_each(a, nlh, sizeof(struct nfgenmsg)) {
			if (mnl_attr_get_type(a) == CTA_STATUS)
				st = ntohl(mnl_attr_get_u32(a));
			else if (mnl_attr_get_type(a) == CTA_STATUS_MASK)
				mask = ntohl(mnl_attr_get_u32(a));
		}
		CHECK(st == (IPS_OFFLOAD | IPS_SEEN_REPLY));
		CHECK(mask == (IPS_OFFLOAD | IPS_SEEN_REPLY | IPS_ASSURED));
		/* exactly the class the other native phases miss */
		CHECK(((IPS_SEEN_REPLY | IPS_OFFLOAD) & mask) == st);			/* selected */
		CHECK(((IPS_SEEN_REPLY | IPS_ASSURED | IPS_OFFLOAD) & mask) != st);	/* phase 2 */
		CHECK(((IPS_OFFLOAD) & mask) != st);					/* phase 1 */
	}
}

/* the per-tuple table: lookup, probing, expiry reuse and eviction */
static void test_rxtable(void)
{
	struct flow f[12];
	struct rx_ent *e;
	unsigned int i, found;

	CHECK(rx_init(8) == 0);
	cnt.rx_evictions = 0;
	for (i = 0; i < 12; i++)
		f[i] = mkflow(IPPROTO_UDP, 1 + i, 443);

	/* fill all eight slots, every tuple stays findable */
	for (i = 0; i < 8; i++) {
		CHECK(rx_find(&f[i]) == NULL);
		e = rx_insert(&f[i], 1001 + i);
		CHECK(e && flow_eq(&e->f, &f[i]) && e->t_rx == 1001 + i && !e->own);
	}
	for (i = 0; i < 8; i++)
		CHECK(rx_find(&f[i]) != NULL);
	CHECK(cnt.rx_evictions == 0);

	/* a ninth live tuple evicts the oldest one */
	e = rx_insert(&f[8], 1050);
	CHECK(e && flow_eq(&e->f, &f[8]));
	CHECK(cnt.rx_evictions == 1);
	CHECK(rx_find(&f[0]) == NULL);
	for (found = 0, i = 1; i < 9; i++)
		found += rx_find(&f[i]) != NULL;
	CHECK(found == 8);

	/* once slots are older than element_timeout they are reused for free */
	e = rx_insert(&f[9], 1200);
	CHECK(e && flow_eq(&e->f, &f[9]));
	CHECK(cnt.rx_evictions == 1);
	CHECK(rx_find(&f[9]) != NULL);
}

/* what an announcement does to a slot */
static void test_classify(void)
{
	struct flow f = mkflow(IPPROTO_UDP, 777, 443);
	struct rx_ent *e;

	CHECK(rx_init(8) == 0);
	e = rx_insert(&f, 1000);
	CHECK(rx_classify(e, 1000) == RX_CREATE);		/* unknown: create with EXCL */
	CHECK(e->own && e->t_inject == 1000 && e->t_rx == 1000);
	/* the create set the copy to element_timeout: the evidence baseline */
	CHECK(e->seen_at == 1000 && e->seen_timeout == cfg.element_timeout);
	/* our copy: only noted, our dump refreshes it */
	CHECK(rx_classify(e, 1010) == RX_NOTED);
	CHECK(e->t_inject == 1000 && e->t_rx == 1010 && e->t_ann == 1010);
	e->own = false;						/* the kernel said EEXIST */
	CHECK(rx_classify(e, 1012) == RX_DUP);			/* within interval/2: once */
	CHECK(rx_classify(e, 1050) == RX_CREATE);
	CHECK(e->own && e->t_ann == 1050);

	/* the dump's refresh decision (element_timeout 90, interval 30) */
	CHECK(refresh_due(e, 60, 1040, 1080) == REFRESH_YES);	/* announced since the last look */
	CHECK(refresh_due(e, 60, 1060, 1080) == REFRESH_NO);	/* not announced since */
	CHECK(refresh_due(e, 7000, 1040, 1080) == REFRESH_HELD);	/* lives on its own traffic */
	CHECK(refresh_due(e, 120, 1040, 1080) == REFRESH_NO);	/* 90 + 30: fresh enough */
	CHECK(refresh_due(e, 89, 1040, 1080) == REFRESH_NO);	/* created a moment ago */
	CHECK(refresh_due(e, 75, 1040, 1080) == REFRESH_NO);	/* 75.x left: < interval/2 ago */
	CHECK(refresh_due(e, 74, 1040, 1080) == REFRESH_YES);
	e->t_ann = 0;						/* re-learned after a restart */
	CHECK(refresh_due(e, 60, 0, 1080) == REFRESH_NO);	/* nobody announced it yet */
}

/* a peer's resync request starts a round, at most once per interval/2 */
static void test_resync(void)
{
	cfg.n_peer = 2;
	resync_round_started();
	resync_from(0);
	CHECK(resync_round_wanted());
	resync_round_started();
	resync_from(0);					/* again at once: ignored */
	CHECK(!resync_round_wanted());
	resync_from(1);					/* another peer: honoured */
	CHECK(resync_round_wanted());
	resync_round_started();
	cfg.n_peer = 0;
}

/* ownership learned from the dump and revoked when the copy is gone */
static void test_seed_sweep(void)
{
	struct flow f = mkflow(IPPROTO_UDP, 888, 443), g = mkflow(IPPROTO_UDP, 889, 443);
	struct rx_ent *e, *e2;

	CHECK(rx_init(8) == 0);
	e = rx_seed(&f, 5, 1000);				/* seen in round 5, slot was unknown */
	CHECK(e && e->own && e->seen_round == 5 && e->t_rx == 1000);
	CHECK(e->t_inject == 1000 - 30);			/* not a duplicate, not swept early */
	rx_sweep(6, 1001);					/* round 6 did not see it: gone */
	CHECK(!e->own && gauge.owned == 0);
	e = rx_seed(&f, 7, 1100);
	CHECK(e->own && e->seen_round == 7 && e->t_rx == 1100);	/* a seen copy keeps its slot */
	rx_sweep(7, 1090);					/* seen this round: kept */
	CHECK(e->own && gauge.owned == 1);
	e2 = rx_insert(&g, 1150);				/* created during round 8 */
	CHECK(rx_classify(e2, 1150) == RX_CREATE && e2->own);
	rx_sweep(8, 1120);
	CHECK(e2->own);						/* too young to have been dumped */
	CHECK(!e->own);						/* old and not seen in round 8 */
	CHECK(gauge.owned == 1);
}

/* copy limit: last round's count plus creates since that round started */
static void test_limit(void)
{
	cfg.max_copies = 100;
	gauge.copies = 0;
	cnt.inject_created = 0;
	rx_round_start();
	rx_sweep(1, 1000);				/* empty round: base 0 */
	CHECK(copies_estimate() == 0 && !rx_limit_reached());
	cnt.inject_created = 60;			/* created since */
	rx_round_start();				/* round 2 starts at 60 created */
	cnt.inject_created = 99;
	CHECK(copies_estimate() == 99 && !rx_limit_reached());
	cnt.inject_created = 100;
	CHECK(rx_limit_reached());
	gauge.copies = 70;				/* round 2 counted 70 copies ... */
	rx_sweep(2, 1030);				/* ... base becomes 60 */
	CHECK(copies_estimate() == 70 + 40 && rx_limit_reached());
	cnt.copies_lost = 30;				/* 30 destroyed early (flush) */
	CHECK(copies_estimate() == 70 + 40 - 30 && !rx_limit_reached());
	rx_round_start();
	gauge.copies = 20;				/* expiries show up in the next count */
	rx_sweep(3, 1060);
	CHECK(copies_estimate() == 20 && !rx_limit_reached());
	rx_disown_all();				/* destroy events lost: unknown */
	CHECK(copies_estimate() == 0);
	cnt.inject_created = 0;
	cnt.copies_lost = 0;
	gauge.copies = 0;
	cfg.max_copies = 0;
	rx_disown_all();
}

/* kernel answers to injected messages and what they do to the bookkeeping */
static void test_account(void)
{
	struct rx_ent e;

	memset(&cnt, 0, sizeof(cnt));
	memset(&e, 0, sizeof(e));

	cnt.inject_created = 2;
	e.own = true;
	CHECK(inj_account(INJ_CREATE, -EEXIST, &e) == ACCT_OK);	/* not ours: hands off */
	CHECK(cnt.inject_created == 1 && cnt.inject_exists == 1 && !e.own);
	e.own = true;
	CHECK(inj_account(INJ_CREATE, -ENOMEM, &e) == ACCT_ERROR);
	CHECK(cnt.inject_created == 0 && cnt.inject_errors == 1 && !e.own);
	CHECK(inj_account(INJ_CREATE, -EEXIST, NULL) == ACCT_OK);	/* unattributed is fine */
	CHECK(cnt.inject_exists == 2);

	cnt.inject_refreshed = 2;
	e.own = true;
	CHECK(inj_account(INJ_REFRESH, -EBUSY, &e) == ACCT_OK);	/* ASSURED copy, still ours */
	CHECK(cnt.inject_refreshed == 2 && e.own);
	CHECK(inj_account(INJ_REFRESH, -ENOENT, &e) == ACCT_RETRY);	/* gone: re-create now */
	CHECK(cnt.inject_refreshed == 1 && cnt.inject_gone == 1 && !e.own);
	e.own = true;
	CHECK(inj_account(INJ_REFRESH, -EINVAL, &e) == ACCT_ERROR);
	CHECK(cnt.inject_refreshed == 0 && cnt.inject_errors == 2 && !e.own);
}

int main(void)
{
	setup_cfg();
	test_prefix();
	test_addr_port();
	test_policy();
	test_wire();
	test_ctnl();
	test_rxtable();
	test_limit();
	test_resync();
	test_classify();
	test_seed_sweep();
	test_account();
	printf("%s: %u checks, %u failures\n", failures ? "FAIL" : "ok", checks, failures);
	return failures ? 1 : 0;
}
