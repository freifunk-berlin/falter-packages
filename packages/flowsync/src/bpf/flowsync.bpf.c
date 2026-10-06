// SPDX-License-Identifier: GPL-2.0-only
/*
 * flowsync datapath: two tc programs on the uplink device.
 *
 * egress   every forwarded IPv6 packet leaving through the uplink keeps its
 *          flow alive in fs_local; the first one of a flow is reported to
 *          the daemon, which announces it to the other gateways.
 * ingress  an IPv6 packet from the uplink whose flow is alive in fs_local or
 *          fs_remote gets the mark; the firewall accepts marked packets and
 *          treats the rest as unsolicited.
 *
 * No verdicts here: both programs let every packet continue. The exception
 * is the bypass (fs_ctl.bypass, off unless the daemon turns it on): ingress
 * then forwards plain TCP and UDP packets of known flows itself, past
 * netfilter and the kernel's forwarding path, see bypass().
 *
 * Fragments: only the first one has the ports. Egress refreshes the flow from
 * it and ignores the others. Ingress marks it; the kernel's reassembly
 * (before the firewall) builds the packet on the first fragment's sk_buff
 * header, so the reassembled packet carries its mark.
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ipv6.h>
#include <linux/pkt_cls.h>
#include <stddef.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#include "../dp.h"

#define MAX_EXT		6	/* extension headers walked before giving up */

#define AF_INET6	10

#define TCP_FIN		0x01
#define TCP_SYN		0x02
#define TCP_RST		0x04
#define TCP_ACK		0x10

/* set by the daemon (dp.c) before the programs are loaded */
const volatile struct fs_cfg cfg = {
	.mark = 0x01000000,
	.t_udp = 180,
	.t_tcp_syn = 120,
	.t_tcp_est = 7440,
	.t_tcp_close = 120,
	.t_other = 600,
};

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 131072);
	__type(key, struct fs_key);
	__type(value, struct fs_local);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} fs_local SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 131072);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, struct fs_key);
	__type(value, struct fs_remote);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} fs_remote SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 1 << 20);
} fs_events SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct fs_stats);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} fs_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct fs_ctl);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} fs_ctl SEC(".maps");

/* what the bypass needs to know about a packet besides its flow */
struct pkt {
	__be32 flowinfo;	/* traffic class and flow label, for the route lookup */
	__u8 hop_limit;
	__u8 plain;		/* the transport header follows the IPv6 header directly */
};

struct ext_hdr {
	__u8 nexthdr;
	__u8 len;
};

struct frag_hdr {
	__u8 nexthdr;
	__u8 reserved;
	__be16 frag_off;
	__be32 id;
};

static __always_inline struct fs_stats *stats(void)
{
	__u32 zero = 0;

	return bpf_map_lookup_elem(&fs_stats, &zero);
}

static __always_inline __u32 now_s(void)
{
	/* seconds are all we need: the tick's time, without reading a clock */
	return bpf_ktime_get_coarse_ns() / 1000000000ULL;
}

/*
 * The flow of a packet, from the network header on whatever the device's
 * link layer is. out: the packet leaves (source is the client). Returns 0
 * with *k filled: addresses and ports for TCP, UDP and SCTP, the addresses
 * alone for every other protocol (ESP, GRE, IP in IP, L2TP, ...). -1 for
 * what has no flow of its own: ICMPv6 (the firewall handles it statelessly),
 * later fragments, an unusable header chain.
 */
static __always_inline int parse(struct __sk_buff *skb, int out, struct fs_key *k, __u8 *tcp_flags,
				 struct pkt *pk)
{
	struct ipv6hdr ip6;
	struct ext_hdr eh;
	struct frag_hdr fh;
	__be16 ports[2] = { 0, 0 };
	__u32 off = sizeof(ip6);
	__u8 nh;
	int i;

	if (bpf_skb_load_bytes_relative(skb, 0, &ip6, sizeof(ip6), BPF_HDR_START_NET))
		return -1;
	if (ip6.version != 6)
		return -1;
	nh = ip6.nexthdr;
	pk->flowinfo = *(__be32 *)&ip6 & bpf_htonl(0x0fffffff);
	pk->hop_limit = ip6.hop_limit;
	pk->plain = nh == IPPROTO_TCP || nh == IPPROTO_UDP;
	for (i = 0; i < MAX_EXT; i++) {
		if (nh == IPPROTO_FRAGMENT) {
			if (bpf_skb_load_bytes_relative(skb, off, &fh, sizeof(fh), BPF_HDR_START_NET))
				return -1;
			if (fh.frag_off & bpf_htons(0xfff8))
				return -1;	/* not the first fragment: no ports */
			nh = fh.nexthdr;
			off += sizeof(fh);
		} else if (nh == IPPROTO_HOPOPTS || nh == IPPROTO_ROUTING || nh == IPPROTO_DSTOPTS) {
			if (bpf_skb_load_bytes_relative(skb, off, &eh, sizeof(eh), BPF_HDR_START_NET))
				return -1;
			nh = eh.nexthdr;
			off += ((__u32)eh.len + 1) * 8;
		} else if (nh == IPPROTO_AH) {
			if (bpf_skb_load_bytes_relative(skb, off, &eh, sizeof(eh), BPF_HDR_START_NET))
				return -1;
			nh = eh.nexthdr;
			off += ((__u32)eh.len + 2) * 4;
		} else {
			break;
		}
	}
	switch (nh) {
	case IPPROTO_FRAGMENT:
	case IPPROTO_HOPOPTS:
	case IPPROTO_ROUTING:
	case IPPROTO_DSTOPTS:
	case IPPROTO_AH:
	case IPPROTO_NONE:
	case IPPROTO_ICMPV6:
		return -1;
	case IPPROTO_TCP:
		if (bpf_skb_load_bytes_relative(skb, off + 13, tcp_flags, 1, BPF_HDR_START_NET))
			return -1;
		/* fall through */
	case IPPROTO_UDP:
	case IPPROTO_SCTP:
		if (bpf_skb_load_bytes_relative(skb, off, ports, sizeof(ports), BPF_HDR_START_NET))
			return -1;
		break;
	}

	__builtin_memset(k, 0, sizeof(*k));
	k->proto = nh;
	if (out) {
		__builtin_memcpy(k->c, &ip6.saddr, 16);
		__builtin_memcpy(k->s, &ip6.daddr, 16);
		k->cport = ports[0];
		k->sport = ports[1];
	} else {
		__builtin_memcpy(k->c, &ip6.daddr, 16);
		__builtin_memcpy(k->s, &ip6.saddr, 16);
		k->cport = ports[1];
		k->sport = ports[0];
	}
	return 0;
}

SEC("tc")
int fs_egress(struct __sk_buff *skb)
{
	struct fs_stats *st = stats();
	struct fs_local *v, nv = {};
	struct fs_key k;
	struct pkt pk;
	__u8 tf = 0, flags;
	__u32 now;

	/* forwarded packets only: what the gateway sends itself stays with
	 * conntrack (OUTPUT) */
	if (skb->protocol != bpf_htons(ETH_P_IPV6) || !skb->ingress_ifindex)
		return TC_ACT_UNSPEC;
	if (parse(skb, 1, &k, &tf, &pk)) {
		if (st)
			st->out_skip++;
		return TC_ACT_UNSPEC;
	}
	if (st)
		st->out_pkts++;
	now = now_s();

	v = bpf_map_lookup_elem(&fs_local, &k);
	if (v) {
		flags = v->flags;
		if (k.proto == IPPROTO_TCP) {
			if ((tf & (TCP_SYN | TCP_ACK)) == TCP_SYN)
				flags = 0;	/* a new connection on the tuple */
			else
				flags |= FS_F_EST;
			if (tf & (TCP_FIN | TCP_RST))
				flags |= FS_F_CLOSING;
			if (flags != v->flags)
				v->flags = flags;
		}
		if (v->seen != now)
			v->seen = now;
		return TC_ACT_UNSPEC;
	}

	nv.seen = now;
	if (k.proto == IPPROTO_TCP) {
		if ((tf & (TCP_SYN | TCP_ACK)) != TCP_SYN)
			nv.flags |= FS_F_EST;	/* picked up mid-stream */
		if (tf & (TCP_FIN | TCP_RST))
			nv.flags |= FS_F_CLOSING;
	}
	bpf_map_update_elem(&fs_local, &k, &nv, BPF_ANY);
	if (st)
		st->out_new++;
	if (bpf_ringbuf_output(&fs_events, &k, sizeof(k), 0) && st)
		st->ev_lost++;
	return TC_ACT_UNSPEC;
}

/*
 * Forward an accepted packet from here: look the route up, lower the hop
 * limit and hand the packet to the neighbour layer of the outgoing device.
 * netfilter, conntrack's hooks and the stack's forwarding path never see it.
 *
 * Only what needs none of them: TCP and UDP directly behind the IPv6 header
 * (so no fragments), no SYN, FIN or RST (the firewall clamps the MSS on SYNs),
 * a hop limit that survives, a route out of another device, and a packet
 * that fits that device. Everything else returns -1 and takes the normal
 * path with its mark, which also sends the ICMPv6 errors there are to send.
 *
 * The uplink must be an Ethernet-like device (the daemon checks): the helper
 * needs a link-layer header to take off, and the hop limit is written at
 * its fixed offset behind it.
 */
static __always_inline int bypass(struct __sk_buff *skb, const struct fs_key *k,
				  const struct pkt *pk, __u8 tf)
{
	struct bpf_fib_lookup fib = {};
	struct bpf_redir_neigh nh = {};
	__u32 mtu = 0;
	__u8 hop;

	if (!pk->plain || pk->hop_limit <= 1)
		return -1;
	if (k->proto == IPPROTO_TCP && (tf & (TCP_SYN | TCP_FIN | TCP_RST)))
		return -1;

	fib.family = AF_INET6;
	fib.ifindex = skb->ingress_ifindex;
	fib.l4_protocol = k->proto;
	fib.sport = k->sport;
	fib.dport = k->cport;
	fib.flowinfo = pk->flowinfo;
	__builtin_memcpy(fib.ipv6_src, k->s, 16);
	__builtin_memcpy(fib.ipv6_dst, k->c, 16);
	/* the neighbour is resolved by the redirect: tunnel devices have none */
	if (bpf_fib_lookup(skb, &fib, sizeof(fib), BPF_FIB_LOOKUP_SKIP_NEIGH) !=
	    BPF_FIB_LKUP_RET_SUCCESS)
		return -1;
	if (fib.ifindex == skb->ingress_ifindex)
		return -1;
	if (bpf_check_mtu(skb, fib.ifindex, &mtu, 0, BPF_MTU_CHK_SEGS))
		return -1;

	hop = pk->hop_limit - 1;
	if (bpf_skb_store_bytes(skb, ETH_HLEN + offsetof(struct ipv6hdr, hop_limit), &hop, 1,
				BPF_F_RECOMPUTE_CSUM))
		return -1;
	nh.nh_family = AF_INET6;
	__builtin_memcpy(nh.ipv6_nh, fib.ipv6_dst, 16);
	return bpf_redirect_neigh(fib.ifindex, &nh, sizeof(nh), 0);
}

static __always_inline int accepted(struct __sk_buff *skb, const struct fs_key *k,
				    const struct pkt *pk, __u8 tf, struct fs_stats *st)
{
	struct fs_ctl *ctl;
	__u32 zero = 0;
	int act;

	skb->mark |= cfg.mark;
	ctl = bpf_map_lookup_elem(&fs_ctl, &zero);
	if (!ctl || !ctl->bypass)
		return TC_ACT_UNSPEC;
	act = bypass(skb, k, pk, tf);
	if (act < 0)
		return TC_ACT_UNSPEC;
	if (st)
		st->in_bypass++;
	return act;
}

SEC("tc")
int fs_ingress(struct __sk_buff *skb)
{
	struct fs_stats *st = stats();
	struct fs_remote *r;
	struct fs_local *v;
	struct fs_key k;
	struct pkt pk;
	__u8 tf = 0;
	__u32 now;

	if (skb->protocol != bpf_htons(ETH_P_IPV6))
		return TC_ACT_UNSPEC;
	/* the mark means "accepted here" and nothing else may bring it in */
	if (skb->mark & cfg.mark)
		skb->mark &= ~cfg.mark;
	if (parse(skb, 0, &k, &tf, &pk)) {
		if (st)
			st->in_skip++;
		return TC_ACT_UNSPEC;
	}
	if (st)
		st->in_pkts++;
	now = now_s();

	v = bpf_map_lookup_elem(&fs_local, &k);
	if (v && now - v->seen <= fs_ttl((const struct fs_cfg *)&cfg, k.proto, v->flags)) {
		if (st)
			st->in_local++;
		return accepted(skb, &k, &pk, tf, st);
	}
	r = bpf_map_lookup_elem(&fs_remote, &k);
	if (r && (__s32)(r->expires - now) > 0) {
		if (st)
			st->in_remote++;
		return accepted(skb, &k, &pk, tf, st);
	}
	if (st)
		st->in_miss++;
	return TC_ACT_UNSPEC;
}

char _license[] SEC("license") = "GPL";
