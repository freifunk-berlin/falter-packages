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
 * then forwards the packets of known flows itself where it can, past
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
#define PACKET_HOST	0

#define TCP_FIN		0x01
#define TCP_SYN		0x02
#define TCP_RST		0x04
#define TCP_ACK		0x10
#define SCTP_ABORT		6
#define SCTP_SHUTDOWN_COMPLETE	14

/* set by the daemon (dp.c) before the programs are loaded: it overwrites
 * the whole section, so these initialisers are documentation, the defaults
 * live in config.c. Read through the volatile only (fs_ttl): a cast away
 * would let the compiler fold them into the code. */
const volatile struct fs_cfg cfg = {
	.mark = 0x01000000,
	.t_udp = 180,
	.t_udp_new = 30,
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
	/* preallocated: a non-preallocated hash refills its per-CPU element
	 * cache from an irq_work, which on MIPS runs from the 100 Hz tick
	 * only, and a resync's burst of inserts would fail with ENOMEM */
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 131072);
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
	__u32 len;		/* of the IPv6 packet, as its header says */
	__u8 hop_limit;
	__u8 ext;		/* extension headers (a fragment header too) before the transport header */
	__u8 frag;		/* one of them a fragment header: the first fragment */
	__u8 answer;		/* a TCP RST, an SCTP ABORT: starts no flow */
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
 * alone for every other protocol (ESP, GRE, IP in IP, L2TP, ...). Negative,
 * one of SKIP_*, for what has no flow of its own.
 */
enum {
	SKIP_ICMPV6 = 1,	/* the firewall handles it statelessly */
	SKIP_LATER_FRAG,	/* not the first fragment */
	SKIP_EXTHDR_CHAIN,	/* more than MAX_EXT extension headers */
	SKIP_OTHER,		/* cut short, not IPv6, no next header */
};
/* parse()'s reason as the index into fs_stats' *_skip_why. An index, not a
 * switch: clang 21 marks the default of a switch over a known range
 * unreachable and then references the kernel function __bpf_trap, which
 * libbpf can only resolve with kernel BTF, and OpenWrt kernels have none. */
#define SKIP_IDX(why)	((-(why) - 1) & (FS_SKIP_WHY - 1))

static __always_inline int parse(struct __sk_buff *skb, int out, struct fs_key *k, __u8 *tcp_flags,
				 struct pkt *pk)
{
	struct ipv6hdr ip6;
	struct ext_hdr eh;
	struct frag_hdr fh;
	__be16 ports[2] = { 0, 0 };
	__u32 off = sizeof(ip6);
	__u8 nh, th[14];
	int i;

	if (bpf_skb_load_bytes_relative(skb, 0, &ip6, sizeof(ip6), BPF_HDR_START_NET))
		return -SKIP_OTHER;
	if (ip6.version != 6)
		return -SKIP_OTHER;
	nh = ip6.nexthdr;
	pk->flowinfo = *(__be32 *)&ip6 & bpf_htonl(0x0fffffff);
	pk->hop_limit = ip6.hop_limit;
	pk->len = bpf_ntohs(ip6.payload_len) + sizeof(ip6);
	pk->frag = 0;
	pk->answer = 0;
	for (i = 0; i < MAX_EXT; i++) {
		if (nh == IPPROTO_FRAGMENT) {
			if (bpf_skb_load_bytes_relative(skb, off, &fh, sizeof(fh), BPF_HDR_START_NET))
				return -SKIP_OTHER;
			if (fh.frag_off & bpf_htons(0xfff8))
				return -SKIP_LATER_FRAG;	/* no ports */
			pk->frag = 1;
			nh = fh.nexthdr;
			off += sizeof(fh);
		} else if (nh == IPPROTO_HOPOPTS || nh == IPPROTO_ROUTING || nh == IPPROTO_DSTOPTS) {
			if (bpf_skb_load_bytes_relative(skb, off, &eh, sizeof(eh), BPF_HDR_START_NET))
				return -SKIP_OTHER;
			nh = eh.nexthdr;
			off += ((__u32)eh.len + 1) * 8;
		} else if (nh == IPPROTO_AH) {
			if (bpf_skb_load_bytes_relative(skb, off, &eh, sizeof(eh), BPF_HDR_START_NET))
				return -SKIP_OTHER;
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
		return -SKIP_EXTHDR_CHAIN;
	case IPPROTO_NONE:
		return -SKIP_OTHER;
	case IPPROTO_ICMPV6:
		return -SKIP_ICMPV6;
	case IPPROTO_TCP:
		/* ports and flags with one load */
		if (bpf_skb_load_bytes_relative(skb, off, th, sizeof(th), BPF_HDR_START_NET))
			return -SKIP_OTHER;
		__builtin_memcpy(ports, th, sizeof(ports));
		*tcp_flags = th[13];
		pk->answer = !!(th[13] & TCP_RST);
		break;
	case IPPROTO_SCTP:
		/* ports and the first chunk's type with one load */
		if (bpf_skb_load_bytes_relative(skb, off, th, 13, BPF_HDR_START_NET))
			return -SKIP_OTHER;
		__builtin_memcpy(ports, th, sizeof(ports));
		pk->answer = th[12] == SCTP_ABORT || th[12] == SCTP_SHUTDOWN_COMPLETE;
		break;
	case IPPROTO_UDP:
		if (bpf_skb_load_bytes_relative(skb, off, ports, sizeof(ports), BPF_HDR_START_NET))
			return -SKIP_OTHER;
		break;
	}

	pk->ext = nh != ip6.nexthdr;
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
	struct fs_stats *st;
	struct fs_local *v, nv = {};
	struct fs_key k;
	struct pkt pk;
	__u8 tf = 0, flags, lost, back;
	__u32 now, age;
	int why;

	/* forwarded packets only: what the gateway sends itself stays with
	 * conntrack (OUTPUT) */
	if (skb->protocol != bpf_htons(ETH_P_IPV6) || !skb->ingress_ifindex)
		return TC_ACT_UNSPEC;
	st = stats();
	why = parse(skb, 1, &k, &tf, &pk);
	if (why) {
		if (st) {
			st->out_skip++;
			st->out_skip_why[SKIP_IDX(why)]++;
		}
		return TC_ACT_UNSPEC;
	}
	if (st)
		st->out_pkts++;
	now = now_s();

	v = bpf_map_lookup_elem(&fs_local, &k);
	if (v) {
		flags = v->flags;
		age = fs_age(now, v->seen);
		/* expired, and still here: the round has not come by yet */
		back = age > fs_ttl(&cfg, k.proto, flags);
		if (back && pk.answer)
			goto answer;
		if (k.proto == IPPROTO_TCP) {
			if ((tf & (TCP_SYN | TCP_ACK)) == TCP_SYN)
				flags = 0;	/* a new connection on the tuple */
			else
				flags |= FS_F_EST;
			if (tf & (TCP_FIN | TCP_RST))
				flags |= FS_F_CLOSING;
		} else if (k.proto == IPPROTO_UDP) {
			if (!back)
				flags |= FS_F_EST;	/* the client sent again */
			else
				flags = 0;	/* a new flow on the tuple */
		}
		if (flags != v->flags)
			v->flags = flags;
		if (v->seen != now)
			v->seen = now;
		if (!back)
			return TC_ACT_UNSPEC;
		/* the peers may have dropped it meanwhile: announced like a new one */
		goto event;
	}

	/*
	 * A RST or an ABORT without a flow is what a host sends for a packet
	 * of no connection: its answer to whoever sent that one, who expects
	 * nothing back. Taken for a flow, it would let that sender in on the
	 * tuple, here and on every peer.
	 */
	if (pk.answer) {
answer:
		if (st)
			st->out_answer++;
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
event:
	lost = bpf_ringbuf_output(&fs_events, &k, sizeof(k), 0) != 0;
	if (st) {
		st->out_new++;
		st->ev_lost += lost;
	}
	return TC_ACT_UNSPEC;
}

/*
 * Forward an accepted packet from here: look the route up, lower the hop
 * limit and hand the packet to the neighbour layer of the outgoing device.
 * netfilter, conntrack's hooks and the stack's forwarding path never see it.
 *
 * Only what needs none of them: a frame addressed to this device (the stack
 * drops what a promiscuous NIC picks up for other hosts; tc sees it first),
 * no extension headers (a router has to look at some of them; and so no
 * fragments: the later ones have no ports and are reassembled on the normal
 * path, the first one must be there with them), no TCP SYN (the firewall
 * clamps its MSS), a hop limit that survives, a route out of another device,
 * and a packet that fits that device. The protocol does not matter: the flow
 * is known, and the firewall would accept the packet on its mark.
 * Everything else is counted by its reason, returns -1 and takes the normal
 * path with its mark, which also sends the ICMPv6 errors there are to send.
 *
 * A redirect that fails after this program returned (no neighbour, a full
 * queue) drops the packet; it stays counted in in_bypass.
 *
 * The uplink must be an Ethernet-like device (the daemon checks): the helper
 * needs a link-layer header to take off, and the hop limit is written at
 * its fixed offset behind it.
 */
#define NORMAL_PATH(why) do { if (st) st->why++; return -1; } while (0)

static __always_inline int bypass(struct __sk_buff *skb, const struct fs_key *k,
				  const struct pkt *pk, __u8 tf, struct fs_stats *st)
{
	struct bpf_fib_lookup fib = {};
	struct bpf_redir_neigh nh = {};
	__u32 mtu = 0;
	__u8 hop;
	long ret;

	if (skb->pkt_type != PACKET_HOST)
		NORMAL_PATH(in_nobypass_other_mac);
	if (pk->frag)
		NORMAL_PATH(in_nobypass_first_frag);
	if (pk->ext)
		NORMAL_PATH(in_nobypass_exthdr);
	if (pk->hop_limit <= 1)
		NORMAL_PATH(in_nobypass_hop_limit);
	if (k->proto == IPPROTO_TCP && (tf & TCP_SYN))
		NORMAL_PATH(in_nobypass_tcp_syn);

	fib.family = AF_INET6;
	fib.ifindex = skb->ingress_ifindex;
	fib.l4_protocol = k->proto;
	fib.sport = k->sport;
	fib.dport = k->cport;
	fib.flowinfo = pk->flowinfo;
	__builtin_memcpy(fib.ipv6_src, k->s, 16);
	__builtin_memcpy(fib.ipv6_dst, k->c, 16);
	/* routing rules that match the packet mark see what they see on the
	 * normal path: the mark as it is here, ours included */
	fib.mark = skb->mark;
	/* Does it fit? Left to themselves both helpers hold skb->len against
	 * the outgoing device's MTU plus its link-layer header, and skb->len
	 * has the uplink's Ethernet header in it here: a full-size packet into
	 * a tunnel device, which has no such header, would be 14 bytes too
	 * long for them. So the lookup gets the packet's own length to hold
	 * against the route's MTU. Not for a GSO packet, whose length is that
	 * of all its segments: the lookup lets it pass, and bpf_check_mtu()
	 * below looks at the segments (by their network length). */
	if (!skb->gso_size)
		fib.tot_len = pk->len;
	/* the neighbour is resolved by the redirect: tunnel devices have none */
	ret = bpf_fib_lookup(skb, &fib, sizeof(fib), BPF_FIB_LOOKUP_SKIP_NEIGH | BPF_FIB_LOOKUP_MARK);
	if (ret != BPF_FIB_LKUP_RET_SUCCESS) {
		if (ret == BPF_FIB_LKUP_RET_FRAG_NEEDED)
			NORMAL_PATH(in_nobypass_mtu);
		NORMAL_PATH(in_nobypass_fib);
	}
	if (fib.ifindex == skb->ingress_ifindex)
		NORMAL_PATH(in_nobypass_same_if);
	if (skb->gso_size && bpf_check_mtu(skb, fib.ifindex, &mtu, 0, BPF_MTU_CHK_SEGS))
		NORMAL_PATH(in_nobypass_mtu);

	/* RECOMPUTE_CSUM stays: ip6_forward() may decrement bare because it
	 * calls skb_forward_csum() first; a redirect does not, and a
	 * CHECKSUM_COMPLETE skb into a veth or bridge would be flagged bad */
	hop = pk->hop_limit - 1;
	if (bpf_skb_store_bytes(skb, ETH_HLEN + offsetof(struct ipv6hdr, hop_limit), &hop, 1,
				BPF_F_RECOMPUTE_CSUM))
		NORMAL_PATH(in_nobypass_hl_write_fail);
	nh.nh_family = AF_INET6;
	__builtin_memcpy(nh.ipv6_nh, fib.ipv6_dst, 16);
	return bpf_redirect_neigh(fib.ifindex, &nh, sizeof(nh), 0);
}

#undef NORMAL_PATH

static __always_inline int accepted(struct __sk_buff *skb, const struct fs_key *k,
				    const struct pkt *pk, __u8 tf, struct fs_stats *st)
{
	struct fs_ctl *ctl;
	__u32 zero = 0;
	int act;

	skb->mark |= cfg.mark;
	ctl = bpf_map_lookup_elem(&fs_ctl, &zero);
	if (!ctl || !ctl->bypass) {
		if (st)
			st->in_nobypass_off++;
		return TC_ACT_UNSPEC;
	}
	act = bypass(skb, k, pk, tf, st);
	if (act < 0)
		return TC_ACT_UNSPEC;
	if (st) {
		st->in_bypass++;
		switch (k->proto) {
		case IPPROTO_TCP:
			st->in_bypass_proto_tcp++;
			break;
		case IPPROTO_UDP:
			st->in_bypass_proto_udp++;
			break;
		case IPPROTO_ESP:
			st->in_bypass_proto_esp++;
			break;
		case IPPROTO_GRE:
			st->in_bypass_proto_gre++;
			break;
		case IPPROTO_IPIP:
			st->in_bypass_proto_ipip++;
			break;
		case IPPROTO_IPV6:
			st->in_bypass_proto_ip6ip6++;
			break;
		case IPPROTO_L2TP:
			st->in_bypass_proto_l2tp++;
			break;
		case IPPROTO_SCTP:
			st->in_bypass_proto_sctp++;
			break;
		default:
			st->in_bypass_proto_other++;
		}
	}
	return act;
}

SEC("tc")
int fs_ingress(struct __sk_buff *skb)
{
	struct fs_stats *st;
	struct fs_remote *r;
	struct fs_local *v;
	struct fs_key k;
	struct pkt pk;
	__u8 tf = 0;
	__u32 now;
	int why;

	if (skb->protocol != bpf_htons(ETH_P_IPV6))
		return TC_ACT_UNSPEC;
	/* the mark means "accepted here" and nothing else may bring it in */
	if (skb->mark & cfg.mark)
		skb->mark &= ~cfg.mark;
	st = stats();
	why = parse(skb, 0, &k, &tf, &pk);
	if (why) {
		if (st) {
			st->in_skip++;
			st->in_skip_why[SKIP_IDX(why)]++;
		}
		return TC_ACT_UNSPEC;
	}
	if (st)
		st->in_pkts++;
	now = now_s();

	v = bpf_map_lookup_elem(&fs_local, &k);
	if (v && fs_age(now, v->seen) <= fs_ttl(&cfg, k.proto, v->flags)) {
		if (st)
			st->in_local++;
		goto accept;
	}
	r = bpf_map_lookup_elem(&fs_remote, &k);
	if (r && (__s32)(r->expires - now) > 0) {
		if (st)
			st->in_remote++;
		goto accept;
	}
	if (st)
		st->in_miss++;
	return TC_ACT_UNSPEC;
accept:	/* one copy of the bypass in the object, not one per lookup */
	return accepted(skb, &k, &pk, tf, st);
}

char _license[] SEC("license") = "GPL";
