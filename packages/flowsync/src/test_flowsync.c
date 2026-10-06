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
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flowsync.h"
#include "dp.h"

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
	buf[WIRE_HDR_LEN] = 1;	/* ICMPv6 is not a known protocol */
	CHECK(wire_get(buf + WIRE_HDR_LEN, &g) == PARSE_ERR);
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
	setup_cfg();
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
	printf("%s: %u checks, %u failures\n", failures ? "FAIL" : "ok", checks, failures);
	return failures ? 1 : 0;
}
