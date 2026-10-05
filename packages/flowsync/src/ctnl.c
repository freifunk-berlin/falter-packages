// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctnetlink message parsing and building with libmnl only. Only the tuple,
 * status, mark and remaining timeout are extracted; nothing is allocated.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <string.h>
#include <libmnl/libmnl.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/netfilter/nf_conntrack_common.h>
#include <linux/netfilter/nf_conntrack_tcp.h>

#include "flowsync.h"

/*
 * CTA_FILTER_ORIG_FLAGS bit: filter the dump on the L4 protocol number.
 * Defined in the kernel's net/netfilter/nf_internals.h (not exported), but
 * part of the ABI and identical in libnetfilter_conntrack.
 */
#define CTA_FILTER_F_CTA_PROTO_NUM	(1 << 3)

struct ct_attrs {
	const struct nlattr *tuple, *status, *mark, *timeout, *id, *protoinfo;
};

struct tuple_attrs {
	const struct nlattr *ip, *proto;
};

struct ip_attrs {
	const struct nlattr *src, *dst;
};

struct proto_attrs {
	const struct nlattr *num, *sport, *dport;
};

static int ct_cb(const struct nlattr *attr, void *data)
{
	struct ct_attrs *a = data;

	switch (mnl_attr_get_type(attr)) {
	case CTA_TUPLE_ORIG:
		if (mnl_attr_validate(attr, MNL_TYPE_NESTED) == 0)
			a->tuple = attr;
		break;
	case CTA_STATUS:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) == 0)
			a->status = attr;
		break;
	case CTA_MARK:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) == 0)
			a->mark = attr;
		break;
	case CTA_TIMEOUT:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) == 0)
			a->timeout = attr;
		break;
	case CTA_ID:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) == 0)
			a->id = attr;
		break;
	case CTA_PROTOINFO:
		if (mnl_attr_validate(attr, MNL_TYPE_NESTED) == 0)
			a->protoinfo = attr;
		break;
	}
	return MNL_CB_OK;
}

/* CTA_PROTOINFO -> CTA_PROTOINFO_TCP -> CTA_PROTOINFO_TCP_STATE */
static int tcp_state_cb(const struct nlattr *attr, void *data)
{
	if (mnl_attr_get_type(attr) == CTA_PROTOINFO_TCP_STATE &&
	    mnl_attr_validate(attr, MNL_TYPE_U8) == 0)
		*(uint8_t *)data = mnl_attr_get_u8(attr);
	return MNL_CB_OK;
}

static int protoinfo_cb(const struct nlattr *attr, void *data)
{
	if (mnl_attr_get_type(attr) == CTA_PROTOINFO_TCP &&
	    mnl_attr_validate(attr, MNL_TYPE_NESTED) == 0)
		mnl_attr_parse_nested(attr, tcp_state_cb, data);
	return MNL_CB_OK;
}

static int tuple_cb(const struct nlattr *attr, void *data)
{
	struct tuple_attrs *a = data;

	switch (mnl_attr_get_type(attr)) {
	case CTA_TUPLE_IP:
		if (mnl_attr_validate(attr, MNL_TYPE_NESTED) == 0)
			a->ip = attr;
		break;
	case CTA_TUPLE_PROTO:
		if (mnl_attr_validate(attr, MNL_TYPE_NESTED) == 0)
			a->proto = attr;
		break;
	}
	return MNL_CB_OK;
}

static int ip_cb(const struct nlattr *attr, void *data)
{
	struct ip_attrs *a = data;

	switch (mnl_attr_get_type(attr)) {
	case CTA_IP_V6_SRC:
		if (mnl_attr_validate2(attr, MNL_TYPE_BINARY, sizeof(struct in6_addr)) == 0)
			a->src = attr;
		break;
	case CTA_IP_V6_DST:
		if (mnl_attr_validate2(attr, MNL_TYPE_BINARY, sizeof(struct in6_addr)) == 0)
			a->dst = attr;
		break;
	}
	return MNL_CB_OK;
}

static int proto_cb(const struct nlattr *attr, void *data)
{
	struct proto_attrs *a = data;

	switch (mnl_attr_get_type(attr)) {
	case CTA_PROTO_NUM:
		if (mnl_attr_validate(attr, MNL_TYPE_U8) == 0)
			a->num = attr;
		break;
	case CTA_PROTO_SRC_PORT:
		if (mnl_attr_validate(attr, MNL_TYPE_U16) == 0)
			a->sport = attr;
		break;
	case CTA_PROTO_DST_PORT:
		if (mnl_attr_validate(attr, MNL_TYPE_U16) == 0)
			a->dport = attr;
		break;
	}
	return MNL_CB_OK;
}

/* original tuple, status, mark, remaining timeout and TCP state of an IPv6
 * entry; -1 if the tuple or the status is missing (mark and timeout default
 * to 0, the TCP state to CT_NO_TCP_STATE) */
int ct_parse(const struct nlmsghdr *nlh, struct ct_entry *e)
{
	const struct nfgenmsg *nfh;
	struct ct_attrs ct = { NULL, NULL, NULL, NULL, NULL, NULL };
	struct tuple_attrs t = { NULL, NULL };
	struct ip_attrs ip = { NULL, NULL };
	struct proto_attrs pr = { NULL, NULL, NULL };

	if (mnl_nlmsg_get_payload_len(nlh) < sizeof(*nfh))
		return -1;
	nfh = mnl_nlmsg_get_payload(nlh);
	if (nfh->nfgen_family != AF_INET6)
		return -1;
	if (mnl_attr_parse(nlh, sizeof(*nfh), ct_cb, &ct) < 0 || !ct.tuple || !ct.status)
		return -1;
	if (mnl_attr_parse_nested(ct.tuple, tuple_cb, &t) < 0 || !t.ip || !t.proto)
		return -1;
	if (mnl_attr_parse_nested(t.ip, ip_cb, &ip) < 0 || !ip.src || !ip.dst)
		return -1;
	if (mnl_attr_parse_nested(t.proto, proto_cb, &pr) < 0 || !pr.num || !pr.sport || !pr.dport)
		return -1;

	memset(e, 0, sizeof(*e));
	memcpy(&e->f.c, mnl_attr_get_payload(ip.src), sizeof(e->f.c));
	memcpy(&e->f.s, mnl_attr_get_payload(ip.dst), sizeof(e->f.s));
	e->f.proto = mnl_attr_get_u8(pr.num);
	e->f.cport = ntohs(mnl_attr_get_u16(pr.sport));
	e->f.sport = ntohs(mnl_attr_get_u16(pr.dport));
	e->status = ntohl(mnl_attr_get_u32(ct.status));
	if (ct.mark)
		e->mark = ntohl(mnl_attr_get_u32(ct.mark));
	if (ct.timeout)
		e->timeout = ntohl(mnl_attr_get_u32(ct.timeout));
	if (ct.id)
		e->id = mnl_attr_get_u32(ct.id);
	e->tcp_state = CT_NO_TCP_STATE;
	if (ct.protoinfo)
		mnl_attr_parse_nested(ct.protoinfo, protoinfo_cb, &e->tcp_state);
	return 0;
}

static struct nfgenmsg *put_nfgenmsg(struct nlmsghdr *nlh)
{
	struct nfgenmsg *nfh = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfh));

	nfh->nfgen_family = AF_INET6;
	nfh->version = NFNETLINK_V0;
	nfh->res_id = 0;
	return nfh;
}

static void put_tuple(struct nlmsghdr *nlh, uint16_t type, const struct in6_addr *src,
		      const struct in6_addr *dst, uint8_t proto, uint16_t sport, uint16_t dport)
{
	struct nlattr *tuple, *nest;

	tuple = mnl_attr_nest_start(nlh, type);
	nest = mnl_attr_nest_start(nlh, CTA_TUPLE_IP);
	mnl_attr_put(nlh, CTA_IP_V6_SRC, sizeof(*src), src);
	mnl_attr_put(nlh, CTA_IP_V6_DST, sizeof(*dst), dst);
	mnl_attr_nest_end(nlh, nest);
	nest = mnl_attr_nest_start(nlh, CTA_TUPLE_PROTO);
	mnl_attr_put_u8(nlh, CTA_PROTO_NUM, proto);
	mnl_attr_put_u16(nlh, CTA_PROTO_SRC_PORT, htons(sport));
	mnl_attr_put_u16(nlh, CTA_PROTO_DST_PORT, htons(dport));
	mnl_attr_nest_end(nlh, nest);
	mnl_attr_nest_end(nlh, tuple);
}

/*
 * TCP needs a state, or the create-time memset leaves it at NONE and the
 * entry is useless. ESTABLISHED, and the per-ct be_liberal flag on both
 * directions so the injected entry accepts the handshake and any out-of-window
 * packet it sees on an asymmetric path without depending on the global
 * nf_conntrack_tcp_be_liberal sysctl. Window scale, seq and sack are not set;
 * the tracker bootstraps the window from the first packet in each direction.
 */
static void put_tcp_protoinfo(struct nlmsghdr *nlh)
{
	struct nf_ct_tcp_flags liberal = {
		.flags = IP_CT_TCP_FLAG_BE_LIBERAL,
		.mask = IP_CT_TCP_FLAG_BE_LIBERAL,
	};
	struct nlattr *proto, *tcp;

	proto = mnl_attr_nest_start(nlh, CTA_PROTOINFO);
	tcp = mnl_attr_nest_start(nlh, CTA_PROTOINFO_TCP);
	mnl_attr_put_u8(nlh, CTA_PROTOINFO_TCP_STATE, TCP_CONNTRACK_ESTABLISHED);
	mnl_attr_put(nlh, CTA_PROTOINFO_TCP_FLAGS_ORIGINAL, sizeof(liberal), &liberal);
	mnl_attr_put(nlh, CTA_PROTOINFO_TCP_FLAGS_REPLY, sizeof(liberal), &liberal);
	mnl_attr_nest_end(nlh, tcp);
	mnl_attr_nest_end(nlh, proto);
}

/*
 * IPCTNL_MSG_CT_NEW for one flow, in two shapes.
 *
 * create: NLM_F_EXCL, both tuples, timeout, status, mark, TCP proto-info. An
 * existing entry is left untouched (EEXIST): that is how a peer's announcement
 * can never refresh an entry this gateway did not create. The kernel sets
 * CONFIRMED on the new entry before it applies CTA_STATUS and refuses any
 * status that differs in that bit, so CONFIRMED must be echoed or the create
 * fails with EBUSY. Never set ASSURED here: a copy must earn it from a packet,
 * that is what tells the copies apart from the flows this gateway forwards.
 *
 * refresh: neither NLM_F_EXCL nor NLM_F_CREATE, both tuples and the timeout,
 * nothing else. The kernel applies the timeout and leaves status, mark and the
 * TCP state alone, so a refresh can never turn a closed or a native entry into
 * something else. On a vanished entry it answers ENOENT. With NLM_F_CREATE it
 * would instead create an unmarked, unreplied entry without TCP state from the
 * refresh: that looks like a native, is announced as one, and drops TCP
 * replies.
 *
 * The caller sets nlmsg_seq.
 */
void ct_build_new(struct nlmsghdr *nlh, const struct flow *f, bool create)
{
	nlh->nlmsg_type = (NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_NEW;
	nlh->nlmsg_flags = NLM_F_REQUEST | (create ? NLM_F_CREATE | NLM_F_EXCL : 0);
	put_nfgenmsg(nlh);
	put_tuple(nlh, CTA_TUPLE_ORIG, &f->c, &f->s, f->proto, f->cport, f->sport);
	put_tuple(nlh, CTA_TUPLE_REPLY, &f->s, &f->c, f->proto, f->sport, f->cport);
	mnl_attr_put_u32(nlh, CTA_TIMEOUT, htonl(cfg.element_timeout));
	if (!create)
		return;
	if (f->proto == IPPROTO_TCP)
		put_tcp_protoinfo(nlh);
	mnl_attr_put_u32(nlh, CTA_STATUS, htonl(IPS_SEEN_REPLY | IPS_CONFIRMED));
	mnl_attr_put_u32(nlh, CTA_MARK, htonl(cfg.ct_mark));
	mnl_attr_put_u32(nlh, CTA_MARK_MASK, htonl(cfg.ct_mark_mask));
}

/*
 * One of this gateway's own native TCP entries to ESTABLISHED, in place: no
 * NLM_F_CREATE, only the original tuple and the proto-info of a copy (state,
 * be_liberal). Status, mark and timeout stay; the next packet sets ASSURED
 * and the established timeout. For an entry stuck in SYN_SENT although
 * replies pass it (see dump_cb in tx.c). The caller sets nlmsg_seq.
 */
void ct_build_promote(struct nlmsghdr *nlh, const struct flow *f)
{
	nlh->nlmsg_type = (NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_NEW;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	put_nfgenmsg(nlh);
	put_tuple(nlh, CTA_TUPLE_ORIG, &f->c, &f->s, f->proto, f->cport, f->sport);
	put_tcp_protoinfo(nlh);
}

/* the same flow seen from the other end: server -> client */
void flow_reverse(struct flow *r, const struct flow *f)
{
	memset(r, 0, sizeof(*r));
	r->c = f->s;
	r->s = f->c;
	r->cport = f->sport;
	r->sport = f->cport;
	r->proto = f->proto;
}

/* field by field: the padding of struct flow is undefined unless the struct
 * was zeroed, so flows are never compared byte for byte */
bool flow_eq(const struct flow *a, const struct flow *b)
{
	return a->proto == b->proto && a->cport == b->cport && a->sport == b->sport &&
	       IN6_ARE_ADDR_EQUAL(&a->c, &b->c) && IN6_ARE_ADDR_EQUAL(&a->s, &b->s);
}

/* the entry that has f as either tuple (the kernel looks up both directions) */
void ct_build_get(struct nlmsghdr *nlh, const struct flow *f)
{
	nlh->nlmsg_type = (NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_GET;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	put_nfgenmsg(nlh);
	put_tuple(nlh, CTA_TUPLE_ORIG, &f->c, &f->s, f->proto, f->cport, f->sport);
}

/* delete the entry with original tuple orig, but only if it is still the one
 * with this id (the kernel compares CTA_ID and answers ENOENT otherwise) */
void ct_build_delete(struct nlmsghdr *nlh, const struct flow *orig, uint32_t id)
{
	nlh->nlmsg_type = (NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_DELETE;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	put_nfgenmsg(nlh);
	put_tuple(nlh, CTA_TUPLE_ORIG, &orig->c, &orig->s, orig->proto, orig->cport, orig->sport);
	mnl_attr_put_u32(nlh, CTA_ID, id);
}

/*
 * Dump request for the IPv6 table, filtered in the kernel on the L4 protocol,
 * optionally on (ct->status & status_mask) == status and on
 * (ct->mark & mark_mask) == mark, so that only the wanted class of entries is
 * serialized and sent to us. A zero mask disables that filter (the kernel
 * rejects an explicit zero status mask). The caller sets nlmsg_seq.
 */
void ct_build_dump(struct nlmsghdr *nlh, uint8_t proto, uint32_t status, uint32_t status_mask,
		   uint32_t mark, uint32_t mark_mask)
{
	struct nlattr *tuple, *nest;

	nlh->nlmsg_type = (NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_GET;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	put_nfgenmsg(nlh);
	if (status_mask) {
		mnl_attr_put_u32(nlh, CTA_STATUS, htonl(status));
		mnl_attr_put_u32(nlh, CTA_STATUS_MASK, htonl(status_mask));
	}
	if (mark_mask) {
		mnl_attr_put_u32(nlh, CTA_MARK, htonl(mark));
		mnl_attr_put_u32(nlh, CTA_MARK_MASK, htonl(mark_mask));
	}
	tuple = mnl_attr_nest_start(nlh, CTA_TUPLE_ORIG);
	nest = mnl_attr_nest_start(nlh, CTA_TUPLE_PROTO);
	mnl_attr_put_u8(nlh, CTA_PROTO_NUM, proto);
	mnl_attr_nest_end(nlh, nest);
	mnl_attr_nest_end(nlh, tuple);
	/* the flags are a host order u32, not be32 */
	nest = mnl_attr_nest_start(nlh, CTA_FILTER);
	mnl_attr_put_u32(nlh, CTA_FILTER_ORIG_FLAGS, CTA_FILTER_F_CTA_PROTO_NUM);
	mnl_attr_put_u32(nlh, CTA_FILTER_REPLY_FLAGS, 0);
	mnl_attr_nest_end(nlh, nest);
}
