/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The flow tables, shared by the tc programs (bpf/flowsync.bpf.c) and the
 * daemon (dp.c).
 *
 * fs_local   flows this gateway forwarded out of its uplink; written by the
 *            egress program only, one entry per flow, `seen` set by every
 *            packet. The daemon reads it (announcements) and deletes what
 *            has expired.
 * fs_remote  flows the peers announced; written by the daemon only, each with
 *            the time it expires at.
 *
 * The ingress program accepts (marks) a packet whose flow is alive in either
 * table, and with fs_ctl.bypass forwards it itself where it can. Nothing else
 * is shared: no entry is ever written by both sides.
 */
#ifndef FLOWSYNC_DP_H
#define FLOWSYNC_DP_H

#include <linux/types.h>

/* client: the address behind the gateways; server: the one outside. Ports in
 * network byte order: TCP, UDP and SCTP have both, every other protocol none
 * (0), its flows are per pair of addresses. */
struct fs_key {
	__u8 c[16];
	__u8 s[16];
	__be16 cport;
	__be16 sport;
	__u8 proto;
	__u8 pad[3];
};

/* what the client's packets told us */
#define FS_F_EST	0x01	/* TCP: a segment other than a SYN, the handshake is past;
				 * UDP: the client sent again */
#define FS_F_CLOSING	0x02	/* TCP: the client sent FIN or RST */

struct fs_local {
	__u32 seen;		/* monotonic seconds of the last packet out */
	__u8 flags;
	__u8 pad[3];
};

struct fs_remote {
	__u32 expires;		/* monotonic seconds */
};

/* the programs' read-only configuration, set by the daemon before loading */
struct fs_cfg {
	__u32 mark;		/* set on accepted packets, cleared on all others */
	__u32 t_udp;		/* seconds after the last packet out; UDP, the client sent again */
	__u32 t_tcp_syn;	/* only SYNs so far */
	__u32 t_tcp_est;
	__u32 t_tcp_close;	/* after the client's FIN or RST */
	__u32 t_other;		/* every other protocol */
	__u32 t_udp_new;	/* UDP, one packet out so far */
};

/* what the daemon may change while the programs run (fs_ctl, one entry) */
struct fs_ctl {
	__u32 bypass;		/* forward accepted packets straight from tc */
	/* the configuration the programs were loaded with, for the flow
	 * commands (the programs read their own copy) */
	struct fs_cfg cfg;
};

#define FS_SKIP_WHY	4	/* a power of two */

/* per CPU */
struct fs_stats {
	__u64 out_pkts;		/* forwarded IPv6 packets out of the uplink, looked at */
	__u64 out_new;		/* of these, first of a flow */
	__u64 out_skip;		/* not parsed: ICMPv6, later fragments, header chain too long */
	__u64 out_answer;	/* a TCP RST or SCTP ABORT without a flow: none made */
	__u64 in_pkts;		/* IPv6 packets in from the uplink, looked at */
	__u64 in_local;		/* accepted on a local flow */
	__u64 in_remote;	/* accepted on a peer's flow */
	__u64 in_bypass;	/* of the accepted ones, forwarded without the stack */
	__u64 in_miss;		/* no flow: left unmarked */
	__u64 in_skip;		/* not parsed, left unmarked (later fragments take the first one's mark) */
	__u64 ev_lost;		/* new flows whose event did not fit the ring */
	/* accepted, but left to the normal path; with in_bypass they add up to
	 * in_local + in_remote */
	__u64 in_nobypass_off;			/* the bypass is switched off */
	__u64 in_nobypass_other_mac;		/* frame addressed to another MAC */
	__u64 in_nobypass_first_frag;		/* a first fragment: the others need it */
	__u64 in_nobypass_exthdr;		/* other extension headers */
	__u64 in_nobypass_tcp_syn;		/* TCP SYN: the firewall clamps its MSS */
	__u64 in_nobypass_hop_limit;		/* hop limit would not survive */
	__u64 in_nobypass_fib;			/* no route to forward on */
	__u64 in_nobypass_same_if;		/* the route leads back out of the uplink */
	__u64 in_nobypass_mtu;			/* does not fit the outgoing device */
	__u64 in_nobypass_hl_write_fail;	/* the hop limit could not be written */
	/* in_bypass by protocol */
	__u64 in_bypass_proto_tcp;
	__u64 in_bypass_proto_udp;
	__u64 in_bypass_proto_esp;
	__u64 in_bypass_proto_gre;
	__u64 in_bypass_proto_ipip;
	__u64 in_bypass_proto_ip6ip6;
	__u64 in_bypass_proto_l2tp;
	__u64 in_bypass_proto_sctp;
	__u64 in_bypass_proto_other;
	/* in_skip and out_skip by reason, in this order: ICMPv6; not the first
	 * fragment (no ports); too many extension headers; the rest (cut short,
	 * not IPv6, no next header) */
	__u64 in_skip_why[FS_SKIP_WHY];
	__u64 out_skip_why[FS_SKIP_WHY];
};

#define FS_PROTO_TCP	6
#define FS_PROTO_UDP	17

/*
 * Seconds since a local flow's last packet out. Never negative: the clock is
 * the tick's, read per packet, and the egress program on another CPU may
 * have written a second that the reader's has not reached yet. Unsigned, that
 * would be an age of 136 years and a live flow taken for expired.
 */
static inline __u32 fs_age(__u32 now, __u32 seen)
{
	__s32 age = now - seen;

	return age > 0 ? age : 0;
}

/* how long a local flow lives after its last packet out. The program passes
 * its `const volatile` configuration: with the volatile cast away the
 * compiler folds the initialisers into the code and the daemon's values
 * are never read (C11 6.7.3p6). */
static inline __u32 fs_ttl(const volatile struct fs_cfg *c, __u8 proto, __u8 flags)
{
	if (proto == FS_PROTO_UDP)
		return flags & FS_F_EST ? c->t_udp : c->t_udp_new;
	if (proto != FS_PROTO_TCP)
		return c->t_other;
	if (flags & FS_F_CLOSING)
		return c->t_tcp_close;
	return flags & FS_F_EST ? c->t_tcp_est : c->t_tcp_syn;
}

#endif
