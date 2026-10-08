// SPDX-License-Identifier: GPL-2.0-only
/*
 * The one nf_tables message the daemon writes itself: the element of the set
 * `alive` in table ip6 flowsync, added with a timeout or taken away. One
 * batch (begin, the message, end), attributes as nft sends them for
 *
 *   add element ip6 flowsync alive { <ifindex> timeout <T>s expires <T>s }
 *   destroy element ip6 flowsync alive { <ifindex> }
 *
 * Adding an element that exists sets its expiration anew (kernel 6.12 and
 * later, the minimum here): that is the heartbeat. Destroy, unlike delete, is
 * no error on a missing element. No library: the daemon links libbpf only,
 * and this is a hundred lines.
 */

#include <endian.h>
#include <string.h>

#include "flowsync.h"
/* after the libc headers: the kernel's in.h yields to them */
#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netlink.h>

struct nlbuf {
	char *p;
	size_t len, cap;
};

static struct nlattr *nla_put(struct nlbuf *b, uint16_t type, const void *data, size_t dlen)
{
	size_t need = NLA_ALIGN(NLA_HDRLEN + dlen);
	struct nlattr *a;

	if (b->len + need > b->cap)
		return NULL;
	a = (struct nlattr *)(b->p + b->len);
	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + dlen;
	if (dlen)
		memcpy((char *)a + NLA_HDRLEN, data, dlen);
	memset(b->p + b->len + NLA_HDRLEN + dlen, 0, need - NLA_HDRLEN - dlen);
	b->len += need;
	return a;
}

static struct nlattr *nla_nest(struct nlbuf *b, uint16_t type)
{
	return nla_put(b, type | NLA_F_NESTED, NULL, 0);
}

static void nla_nest_end(struct nlbuf *b, struct nlattr *a)
{
	a->nla_len = b->p + b->len - (char *)a;
}

static struct nlmsghdr *nl_msg(struct nlbuf *b, uint16_t type, uint16_t flags, uint32_t seq,
			       uint32_t port, uint8_t family, uint16_t res_id)
{
	struct nlmsghdr *h = (struct nlmsghdr *)(b->p + b->len);
	struct nfgenmsg *g;

	if (b->len + NLMSG_SPACE(sizeof(*g)) > b->cap)
		return NULL;
	memset(h, 0, NLMSG_SPACE(sizeof(*g)));
	h->nlmsg_len = NLMSG_LENGTH(sizeof(*g));
	h->nlmsg_type = type;
	h->nlmsg_flags = flags;
	h->nlmsg_seq = seq;
	h->nlmsg_pid = port;
	g = NLMSG_DATA(h);
	g->nfgen_family = family;
	g->version = NFNETLINK_V0;
	g->res_id = htobe16(res_id);
	b->len += NLMSG_SPACE(sizeof(*g));
	return h;
}

static void nl_msg_end(struct nlbuf *b, struct nlmsghdr *h)
{
	h->nlmsg_len = b->p + b->len - (char *)h;
}

/*
 * Build the batch into buf: begin (seq), the element (seq + NFNL_ALIVE_ACKED,
 * the message that asks for the ack), end. Returns the length, 0 if it does
 * not fit.
 */
size_t nfnl_alive_msg(char *buf, size_t cap, bool add, unsigned int ifindex,
		      unsigned long timeout_s, uint32_t seq, uint32_t port)
{
	struct nlbuf b = { buf, 0, cap };
	struct nlmsghdr *m;
	struct nlattr *elems, *elem, *key;
	uint64_t be = htobe64((uint64_t)timeout_s * 1000);	/* big endian on the wire */
	uint32_t idx = ifindex;
	uint16_t type = (NFNL_SUBSYS_NFTABLES << 8) |
			(add ? NFT_MSG_NEWSETELEM : NFT_MSG_DESTROYSETELEM);

	if (!nl_msg(&b, NFNL_MSG_BATCH_BEGIN, NLM_F_REQUEST, seq++, port, AF_UNSPEC,
		    NFNL_SUBSYS_NFTABLES))
		return 0;
	m = nl_msg(&b, type, NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE : 0), seq++, port,
		   NFPROTO_IPV6, 0);
	if (!m ||
	    !nla_put(&b, NFTA_SET_ELEM_LIST_TABLE, NFNL_TABLE, sizeof(NFNL_TABLE)) ||
	    !nla_put(&b, NFTA_SET_ELEM_LIST_SET, NFNL_ALIVE_SET, sizeof(NFNL_ALIVE_SET)) ||
	    !(elems = nla_nest(&b, NFTA_SET_ELEM_LIST_ELEMENTS)) ||
	    !(elem = nla_nest(&b, NFTA_LIST_ELEM)) ||
	    !(key = nla_nest(&b, NFTA_SET_ELEM_KEY)) ||
	    !nla_put(&b, NFTA_DATA_VALUE, &idx, sizeof(idx)))
		return 0;
	nla_nest_end(&b, key);
	if (add && (!nla_put(&b, NFTA_SET_ELEM_TIMEOUT, &be, sizeof(be)) ||
		    !nla_put(&b, NFTA_SET_ELEM_EXPIRATION, &be, sizeof(be))))
		return 0;
	nla_nest_end(&b, elem);
	nla_nest_end(&b, elems);
	nl_msg_end(&b, m);
	if (!nl_msg(&b, NFNL_MSG_BATCH_END, NLM_F_REQUEST, seq, port, AF_UNSPEC,
		    NFNL_SUBSYS_NFTABLES))
		return 0;
	return b.len;
}
