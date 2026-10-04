// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctquery - look at the IPv6 conntrack table from the flowsync tests. Built
 * against the daemon's own ctnetlink code, libmnl only.
 *
 *   ctquery get <proto> <client> <cport> <server> <sport>
 *       one line for the entry with that original tuple, or "none":
 *       timeout=<remaining>s UNREPLIED|SEEN_REPLY [ASSURED] [OFFLOAD] mark=0x.. [tcp_state=N]
 *   ctquery count <proto> all|marked|native
 *       count=<entries> assured=<of which ASSURED>, filtered on the flowsync mark
 *   ctquery flush
 *       delete every IPv6 entry
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

static struct mnl_socket *nl;
static unsigned int portid;
static uint32_t seq;

struct got {
	struct ct_entry e;
	int tcp_state;
	bool found;
};

struct tally {
	unsigned int n, assured;
};

static int tcp_cb(const struct nlattr *a, void *d)
{
	if (mnl_attr_get_type(a) == CTA_PROTOINFO_TCP_STATE)
		((struct got *)d)->tcp_state = mnl_attr_get_u8(a);
	return MNL_CB_OK;
}

static int protoinfo_cb(const struct nlattr *a, void *d)
{
	if (mnl_attr_get_type(a) == CTA_PROTOINFO_TCP)
		mnl_attr_parse_nested(a, tcp_cb, d);
	return MNL_CB_OK;
}

static int top_cb(const struct nlattr *a, void *d)
{
	if (mnl_attr_get_type(a) == CTA_PROTOINFO)
		mnl_attr_parse_nested(a, protoinfo_cb, d);
	return MNL_CB_OK;
}

static int get_cb(const struct nlmsghdr *nlh, void *d)
{
	struct got *g = d;

	if (ct_parse(nlh, &g->e) == 0) {
		g->found = true;
		g->tcp_state = -1;
		mnl_attr_parse(nlh, sizeof(struct nfgenmsg), top_cb, g);
	}
	return MNL_CB_OK;
}

static int count_cb(const struct nlmsghdr *nlh, void *d)
{
	struct tally *t = d;
	struct ct_entry e;

	if (ct_parse(nlh, &e) == 0) {
		t->n++;
		if (e.status & IPS_ASSURED)
			t->assured++;
	}
	return MNL_CB_OK;
}

static struct nlmsghdr *hdr(char *buf, uint8_t msg, uint16_t flags)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct nfgenmsg *nfh;

	nlh->nlmsg_type = (NFNL_SUBSYS_CTNETLINK << 8) | msg;
	nlh->nlmsg_flags = NLM_F_REQUEST | flags;
	nfh = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfh));
	nfh->nfgen_family = AF_INET6;
	nfh->version = NFNETLINK_V0;
	nfh->res_id = 0;
	return nlh;
}

static void put_tuple(struct nlmsghdr *nlh, const struct flow *f)
{
	struct nlattr *t = mnl_attr_nest_start(nlh, CTA_TUPLE_ORIG), *n;

	n = mnl_attr_nest_start(nlh, CTA_TUPLE_IP);
	mnl_attr_put(nlh, CTA_IP_V6_SRC, 16, &f->c);
	mnl_attr_put(nlh, CTA_IP_V6_DST, 16, &f->s);
	mnl_attr_nest_end(nlh, n);
	n = mnl_attr_nest_start(nlh, CTA_TUPLE_PROTO);
	mnl_attr_put_u8(nlh, CTA_PROTO_NUM, f->proto);
	mnl_attr_put_u16(nlh, CTA_PROTO_SRC_PORT, htons(f->cport));
	mnl_attr_put_u16(nlh, CTA_PROTO_DST_PORT, htons(f->sport));
	mnl_attr_nest_end(nlh, n);
	mnl_attr_nest_end(nlh, t);
}

/* send one request and run cb over the answer until done; returns -errno */
static int xact(struct nlmsghdr *nlh, mnl_cb_t cb, void *data)
{
	static char buf[65536] __attribute__((aligned(8)));
	int ret;

	nlh->nlmsg_seq = ++seq;
	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0)
		return -errno;
	for (;;) {
		ssize_t n = mnl_socket_recvfrom(nl, buf, sizeof(buf));

		if (n < 0)
			return -errno;
		ret = mnl_cb_run(buf, n, seq, portid, cb, data);
		if (ret <= MNL_CB_STOP)
			break;
	}
	return ret < 0 ? -errno : 0;
}

static int cmd_get(char **argv)
{
	char buf[1024] __attribute__((aligned(8)));
	struct nlmsghdr *nlh = hdr(buf, IPCTNL_MSG_CT_GET, NLM_F_ACK);
	struct flow f;
	struct got g;
	int p = proto_num(argv[0]);
	int rc;

	memset(&f, 0, sizeof(f));
	memset(&g, 0, sizeof(g));
	if (p < 0 || inet_pton(AF_INET6, argv[1], &f.c) != 1 || parse_port(argv[2], &f.cport) ||
	    inet_pton(AF_INET6, argv[3], &f.s) != 1 || parse_port(argv[4], &f.sport)) {
		fprintf(stderr, "bad tuple\n");
		return 2;
	}
	f.proto = p;
	put_tuple(nlh, &f);
	rc = xact(nlh, get_cb, &g);
	if (rc == -ENOENT || !g.found) {
		printf("none\n");
		return 1;
	}
	if (rc) {
		printf("error: %s\n", strerror(-rc));
		return 2;
	}
	printf("timeout=%us %s%s%s mark=0x%x", g.e.timeout,
	       g.e.status & IPS_SEEN_REPLY ? "SEEN_REPLY" : "UNREPLIED",
	       g.e.status & IPS_ASSURED ? " ASSURED" : "",
	       g.e.status & IPS_OFFLOAD ? " OFFLOAD" : "", g.e.mark);
	if (f.proto == IPPROTO_TCP)
		printf(" tcp_state=%d", g.tcp_state);
	printf("\n");
	return 0;
}

static int cmd_count(char **argv)
{
	char buf[1024] __attribute__((aligned(8)));
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);	/* ct_build_dump adds the nfgenmsg */
	struct tally t = { 0, 0 };
	int p = proto_num(argv[0]);
	uint32_t mark = 0, mask = 0;
	int rc;

	if (p < 0) {
		fprintf(stderr, "bad protocol\n");
		return 2;
	}
	if (!strcmp(argv[1], "marked")) {
		mark = cfg.ct_mark;
		mask = cfg.ct_mark_mask;
	} else if (!strcmp(argv[1], "native")) {
		mask = cfg.ct_mark_mask;
	} else if (strcmp(argv[1], "all")) {
		fprintf(stderr, "all|marked|native\n");
		return 2;
	}
	ct_build_dump(nlh, p, 0, 0, mark, mask);
	rc = xact(nlh, count_cb, &t);
	if (rc) {
		printf("error: %s\n", strerror(-rc));
		return 2;
	}
	printf("count=%u assured=%u\n", t.n, t.assured);
	return 0;
}

static int cmd_flush(void)
{
	char buf[256] __attribute__((aligned(8)));
	struct nlmsghdr *nlh = hdr(buf, IPCTNL_MSG_CT_DELETE, NLM_F_ACK);
	int rc = xact(nlh, NULL, NULL);

	if (rc) {
		printf("error: %s\n", strerror(-rc));
		return 2;
	}
	printf("flushed\n");
	return 0;
}

int main(int argc, char **argv)
{
	memset(&cfg, 0, sizeof(cfg));
	cfg.ct_mark = 0x01000000;
	cfg.ct_mark_mask = 0x01000000;
	cfg.element_timeout = 90;

	nl = mnl_socket_open(NETLINK_NETFILTER);
	if (!nl || mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		perror("netlink");
		return 2;
	}
	portid = mnl_socket_get_portid(nl);

	if (argc == 7 && !strcmp(argv[1], "get"))
		return cmd_get(argv + 2);
	if (argc == 4 && !strcmp(argv[1], "count"))
		return cmd_count(argv + 2);
	if (argc == 2 && !strcmp(argv[1], "flush"))
		return cmd_flush();
	fprintf(stderr, "usage: ctquery get <proto> <client> <cport> <server> <sport>\n"
			"       ctquery count <proto> all|marked|native\n"
			"       ctquery flush\n");
	return 2;
}
